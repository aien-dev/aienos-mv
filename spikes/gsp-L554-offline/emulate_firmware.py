#!/usr/bin/env python3
"""Offline RV64 binary experiment. No passthrough, CSR fakery or GPU devices.
The boot prefix is an explicit entry/base/initial-state hypothesis, not a boot.
The leaf routine is invoked through an inferred ABI, not symbol-name recovery.
"""
import argparse, hashlib, json, random, struct
from pathlib import Path
import capstone, unicorn
from unicorn import riscv_const as rv

PIN='32cb7cf3e77f97e92f6e92135939ccb3ba382748144569c0dfccf3338d43d963'
MASK=(1<<64)-1
BASE=0x10000000
STOP=0x20000000
md=capstone.Cs(capstone.CS_ARCH_RISCV,capstone.CS_MODE_RISCV64|capstone.CS_MODE_RISCVC)

def machine(image):
    uc=unicorn.Uc(unicorn.UC_ARCH_RISCV,unicorn.UC_MODE_RISCV64)
    uc.mem_map(BASE,(len(image)+4095)&~4095)
    uc.mem_write(BASE,image)
    for i in range(1,32):uc.reg_write(getattr(rv,'UC_RISCV_REG_X'+str(i)),0)
    return uc

def boot(image):
    uc=machine(image);trace=[];reason={};mem=[]
    def code(uc,pc,size,_):
        data=bytes(uc.mem_read(pc,size));dec=list(md.disasm(data,pc))
        row=dict(pc=pc,offset=pc-BASE,bytes=data.hex(),assembly=(dec[0].mnemonic+' '+dec[0].op_str) if dec else 'UNKNOWN')
        trace.append(row)
        word=int.from_bytes(data,'little')
        if size==4 and word&127==0x73:
            reason.update(kind='SYSTEM_INSTRUCTION_BOUNDARY',pc=pc,offset=pc-BASE,word_hex=hex(word),csr=word>>20,assembly=row['assembly']);uc.emu_stop()
        elif len(trace)>=10000:
            reason.update(kind='INSTRUCTION_LIMIT',pc=pc);uc.emu_stop()
    def invalid(uc,access,address,size,value,_):
        reason.update(kind='UNMAPPED_MEMORY',access=access,address=address,size=size,value=value);return False
    def access(uc,kind,address,size,value,_):
        if len(mem)<64:mem.append(dict(kind=kind,address=address,size=size,value=value))
        if not(BASE<=address and address+size<=BASE+len(image)):
            reason.update(kind='OUTSIDE_IMAGE_BYTES',address=address,size=size);uc.emu_stop()
    uc.hook_add(unicorn.UC_HOOK_CODE,code)
    uc.hook_add(unicorn.UC_HOOK_MEM_INVALID,invalid)
    uc.hook_add(unicorn.UC_HOOK_MEM_READ|unicorn.UC_HOOK_MEM_WRITE,access)
    try:uc.emu_start(BASE,STOP,timeout=1000000,count=10000)
    except unicorn.UcError as ex:reason.setdefault('emulator_error',str(ex))
    assert reason,'No explicit stopping boundary'
    return dict(assumptions={'base':BASE,'entry_offset':0,'initial_registers':'zero','memory':'copied image only; rounded page padding refused on access'},instructions_observed=len(trace),instructions_before_boundary=max(0,len(trace)-1),stop=reason,trace=trace,memory=mem,final_registers={str(i):uc.reg_read(getattr(rv,'UC_RISCV_REG_X'+str(i))) for i in range(32)})

def cases():
    edge=[0,1,2,7,8,(1<<63)-1,1<<63,MASK-1,MASK]
    result=[(a,n,b,m) for a in edge for n in edge for b in edge for m in edge]
    rng=random.Random(554)
    for _ in range(2000):
        a,n,b,m=(rng.getrandbits(64) for i in range(4));result.append((a,n,b,m))
    for _ in range(2000):
        a,b=rng.getrandbits(16),rng.getrandbits(16);n,m=rng.getrandbits(12),rng.getrandbits(12);result.append((a,n,b,m))
    return result

def expected(a,n,b,m):
    # Unbounded mathematical endpoint comparison, independent of RV instruction decoding.
    return int(n!=0 and m!=0 and (a+n>MASK or b+m>MASK or max(a,b)<min(a+n,b+m)))

def leaf(image,out):
    uc=machine(image[:4096]);uc.mem_protect(BASE,4096,unicorn.UC_PROT_READ|unicorn.UC_PROT_EXEC)
    # Unicorn fetches at the return sentinel before its end-PC check. Map a
    # separate non-device page; no sentinel instruction is executed.
    uc.mem_map(STOP,4096,unicorn.UC_PROT_READ|unicorn.UC_PROT_EXEC)
    observed=[];edge_cases=cases()
    for a,n,b,m in edge_cases:
        for i in range(1,32):uc.reg_write(getattr(rv,'UC_RISCV_REG_X'+str(i)),0)
        for reg,val in [(rv.UC_RISCV_REG_A0,a),(rv.UC_RISCV_REG_A1,n),(rv.UC_RISCV_REG_A2,b),(rv.UC_RISCV_REG_A3,m),(rv.UC_RISCV_REG_RA,STOP)]:uc.reg_write(reg,val)
        uc.emu_start(BASE+0x28,STOP,timeout=100000,count=64)
        assert uc.reg_read(rv.UC_RISCV_REG_PC)==STOP,'Did not return within instruction budget'
        got=uc.reg_read(rv.UC_RISCV_REG_A0);want=expected(a,n,b,m)
        assert got==want,dict(args=[a,n,b,m],got=got,want=want)
        observed.append(got)
    ib=b''.join(struct.pack('<QQQQ',*r) for r in edge_cases);ob=b''.join(struct.pack('<Q',x) for x in observed)
    (out/'leaf-input.bin').write_bytes(ib);(out/'leaf-expected.bin').write_bytes(ob)
    return dict(entry_offset=0x28,code_extent=[0x28,0x54],code_sha256=hashlib.sha256(image[0x28:0x54]).hexdigest(),inferred_contract='zero-length => 0; unsigned endpoint overflow => 1; otherwise half-open range intersection',cases=len(observed),ones=sum(observed),zeros=len(observed)-sum(observed),input_sha256=hashlib.sha256(ib).hexdigest(),expected_sha256=hashlib.sha256(ob).hexdigest(),result='BINARY_LEAF_EQUIVALENCE_PASS',limits='One pure routine; not proof of full firmware functionality, symbol identity, firmware boot, or hardware admission')

def main():
    ap=argparse.ArgumentParser();ap.add_argument('snapshot',type=Path);a=ap.parse_args();p=a.snapshot
    original=(p/'gsp_ga10x.bin').read_bytes();assert hashlib.sha256(original).hexdigest()==PIN
    image=(p/'fwimage.bin').read_bytes();mapping=json.loads((p/'binary-map.json').read_text())
    assert hashlib.sha256(image).hexdigest()==mapping['fwimage_sha256']
    result=dict(unicorn=unicorn.__version__,capstone=capstone.__version__,firmware_sha256=PIN,boot=boot(image),leaf=leaf(image,p))
    (p/'emulation.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(dict(boot_stop=result['boot']['stop'],boot_instructions_observed=result['boot']['instructions_observed'],leaf=result['leaf']),indent=2))

if __name__=='__main__':main()
