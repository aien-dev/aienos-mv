#!/usr/bin/env python3
"""External stress qualification: real RV64 bytes in QEMU+Unicorn vs native Omega.
No GPU access. Large binary corpora/executables stay in the private lab directory.
"""
import collections,hashlib,io,itertools,json,os,random,struct,subprocess,sys
from pathlib import Path
from elftools.elf.elffile import ELFFile
import unicorn
from unicorn import riscv_const as rv

MASK=(1<<64)-1
BASE=0x10000000;STOP=0x20000000
FW_SHA='32cb7cf3e77f97e92f6e92135939ccb3ba382748144569c0dfccf3338d43d963'
COMPILER_SHA='01b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6'
LEAF_SHA='a5d388a59f99f2e5afe44f3dab0b7eb3ad8832240e17ece7e4bc4555728996f5'

def sha(b):return hashlib.sha256(b).hexdigest()
def oracle(a,n,b,m):
    if n==0 or m==0:return 0
    if a+n>MASK or b+m>MASK:return 1
    return int(not(a+n<=b or b+m<=a))

def generate(old,proof):
    rows=list(struct.iter_unpack('<QQQQ',old));groups=['original']*len(rows)
    def add(tag,vals):
        assert len(vals)==4 and all(0<=v<=MASK for v in vals)
        rows.append(tuple(vals));groups.append(tag)
    for vals in itertools.product(range(16),repeat=4):
        a,n,b,m=vals
        add('exhaustive-low',vals);add('exhaustive-high',(MASK-a,n,MASK-b,m));add('exhaustive-mixed',(MASK-a,n,b,m))
    for bit in range(64):
        for d in range(-2,3):
            a=((1<<bit)+d)&MASK
            for n in (0,1,2,15,1<<bit):
                for delta in (-1,0,1):add('power-boundaries',(a,n,(a+n+delta)&MASK,n))
    for q in proof['comparisons']:
        if 'counterexample' in q:add('solver-counterexamples',q['counterexample'])
    tags=['empty-a','empty-b','wrap-a','wrap-b','adjacent','overlap','disjoint','near-max','full-width','identical','contained']
    rngs={tag:random.Random(55400+i) for i,tag in enumerate(tags)}
    while len(rows)<1000000:
        tag=tags[(len(rows)-len(old)//32)%len(tags)];r=rngs[tag]
        a,n,b,m=[r.getrandbits(64) for _ in range(4)]
        if tag=='empty-a':n=0
        elif tag=='empty-b':m=0
        elif tag=='wrap-a':a|=1;n=MASK-a+1+r.randrange(a);m=m or 1
        elif tag=='wrap-b':b|=1;m=MASK-b+1+r.randrange(b);n=n or 1
        elif tag in ('adjacent','overlap','disjoint','contained'):
            a>>=1;n=r.randrange(2,65536);m=r.randrange(1,65536)
            if tag=='adjacent':b=a+n
            elif tag=='overlap':b=a+r.randrange(n)
            elif tag=='disjoint':b=a+n+r.randrange(1,65536)
            else:b=a+r.randrange(n);m=r.randrange(1,a+n-b+1)
        elif tag=='near-max':a=MASK-r.randrange(512);b=MASK-r.randrange(512);n=r.randrange(1024);m=r.randrange(1024)
        elif tag=='identical':b=a;m=n
        add(tag,(a,n,b,m))
    # Metamorphic pairs use the full original corpus; no count hidden in a single case.
    pairs=[]
    for ix,args in enumerate(list(rows[:len(old)//32])):
        a,n,b,m=args;j=len(rows);add('symmetry',(b,m,a,n));pairs.append((ix,j,'symmetry'))
        if a+n<=MASK and b+m<=MASK:
            gap=MASK-max(a+n,b+m);delta=min(gap,65537);j=len(rows);add('translation',(a+delta,n,b+delta,m));pairs.append((ix,j,'translation'))
    return rows,groups,pairs

def build_qemu(work,adapter,image):
    tools=work/'tools';env={**os.environ,'LD_LIBRARY_PATH':str(tools/'usr/lib/aarch64-linux-gnu')}
    leaf=image[0x28:0x54];assert sha(leaf)==LEAF_SHA;(work/'leaf.bin').write_bytes(leaf)
    src=work/'qemu-adapter.S';src.write_bytes(adapter.read_bytes());obj=work/'qemu-adapter.o';exe=work/'qemu-oracle.elf'
    commands=[[str(tools/'usr/bin/riscv64-linux-gnu-as'),'-march=rv64imac','-mabi=lp64','-o',str(obj),str(src)],
              [str(tools/'usr/bin/riscv64-linux-gnu-ld'),'-m','elf64lriscv','-static','-e','_start','-o',str(exe),str(obj)]]
    for cmd in commands:
        p=subprocess.run(cmd,cwd=work,env=env,capture_output=True,text=True,timeout=30);assert p.returncode==0,(cmd,p.stderr)
    elf=ELFFile(io.BytesIO(exe.read_bytes()));assert elf.header.e_machine=='EM_RISCV'
    syms=elf.get_section_by_name('.symtab');start=syms.get_symbol_by_name('leaf')[0]['st_value'];end=syms.get_symbol_by_name('leaf_end')[0]['st_value']
    sec=elf.get_section_by_name('.text');linked=sec.data()[start-sec['sh_addr']:end-sec['sh_addr']];assert linked==leaf
    qemu=tools/'usr/bin/qemu-riscv64';version=subprocess.check_output([str(qemu),'--version'],text=True).splitlines()[0]
    for payload,want,rc in [(bytes(32),bytes(8),0),(struct.pack('<QQQQ',0,1,0,1),struct.pack('<Q',1),0),(bytes(31),b'',2)]:
        smoke=subprocess.run([str(qemu),str(exe)],input=payload,capture_output=True,timeout=10)
        assert (smoke.returncode,smoke.stdout,smoke.stderr)==(rc,want,b''),'QEMU adapter smoke failed before corpus'
    return [str(qemu),str(exe)],dict(commands=commands,adapter_sha256=sha(src.read_bytes()),executable_sha256=sha(exe.read_bytes()),leaf_sha256=sha(linked),leaf_va=start,version=version,qemu_sha256=sha(qemu.read_bytes()))

def build_omega(work,compiler,source,name='omega-stress'):
    assert sha(compiler.read_bytes())==COMPILER_SHA
    p=subprocess.run([str(compiler)],input=source.encode(),capture_output=True,timeout=30)
    assert p.returncode==0 and not p.stderr,(p.returncode,p.stderr)
    assert len(p.stdout)>64 and p.stdout[:4]==b'\x7fELF' and struct.unpack_from('<HH',p.stdout,16)==(2,183)
    f=work/(name+'.elf');f.write_bytes(p.stdout);f.chmod(0o755)
    return [str(f)],dict(compile_rc=p.returncode,source_sha256=sha(source.encode()),executable_sha256=sha(p.stdout))

def run_command(cmd,inputs,expected):
    p=subprocess.run(cmd,input=inputs,capture_output=True,timeout=120)
    assert p.returncode==0 and not p.stderr,(cmd,p.returncode,p.stderr[:1000])
    assert len(p.stdout)==len(expected),(len(p.stdout),len(expected))
    if p.stdout!=expected:
        mismatch=next(i for i,(a,b) in enumerate(zip(struct.iter_unpack('<Q',p.stdout),struct.iter_unpack('<Q',expected))) if a!=b)
        raise AssertionError((cmd,'mismatch at record',mismatch))
    return dict(cases=len(p.stdout)//8,exit=p.returncode,output_sha256=sha(p.stdout))

def run_unicorn(image,rows,expected,work):
    u=unicorn.Uc(unicorn.UC_ARCH_RISCV,unicorn.UC_MODE_RISCV64)
    u.mem_map(BASE,4096);u.mem_write(BASE,image[:4096]);u.mem_protect(BASE,4096,unicorn.UC_PROT_READ|unicorn.UC_PROT_EXEC);u.mem_map(STOP,4096,unicorn.UC_PROT_READ|unicorn.UC_PROT_EXEC)
    u.reg_write(rv.UC_RISCV_REG_RA,STOP)
    output=bytearray(len(rows)*8);argregs=[rv.UC_RISCV_REG_A0,rv.UC_RISCV_REG_A1,rv.UC_RISCV_REG_A2,rv.UC_RISCV_REG_A3]
    for ix,args in enumerate(rows):
        for reg,val in zip(argregs,args):u.reg_write(reg,val)
        u.emu_start(BASE+0x28,STOP,count=64)
        assert u.reg_read(rv.UC_RISCV_REG_PC)==STOP,('budget/return failure',ix)
        got=u.reg_read(rv.UC_RISCV_REG_A0);want=struct.unpack_from('<Q',expected,ix*8)[0]
        assert got==want,(ix,args,got,want)
        struct.pack_into('<Q',output,ix*8,got)
        if (ix+1)%100000==0:
            (work/'unicorn-progress.json').write_text(json.dumps(dict(completed=ix+1,total=len(rows)))+'\n');print('UNICORN',ix+1,flush=True)
    assert bytes(output)==expected
    return dict(cases=len(rows),version=unicorn.__version__,output_sha256=sha(output),return_budget=64)

def main():
    source=Path(sys.argv[1]);lab=Path(sys.argv[2]);work=lab/'stress';snap=lab/'snapshot-20261022T212950Z';work.mkdir(exist_ok=True)
    report=dict(status='RUNNING',base_source_pin='0f97c7c945cc9b96d250ffce0a25b82beadebc43',source_sha256={p.name:sha(p.read_bytes()) for p in sorted(source.iterdir()) if p.suffix in ('.py','.om','.S')})
    try:
        raw=(snap/'gsp_ga10x.bin').read_bytes();assert sha(raw)==FW_SHA;image=(snap/'fwimage.bin').read_bytes()
        proof=json.loads((work/'symbolic-proof.json').read_text());rows,groups,pairs=generate((snap/'leaf-input.bin').read_bytes(),proof)
        inputs=b''.join(struct.pack('<QQQQ',*r) for r in rows);wanted=[oracle(*r) for r in rows];expected=b''.join(struct.pack('<Q',v) for v in wanted)
        for a,b,tag in pairs:assert wanted[a]==wanted[b],tag
        assert all(tag in groups for tag in ['empty-a','empty-b','wrap-a','wrap-b','adjacent','overlap','disjoint','near-max','full-width','identical','contained'])
        (work/'corpus.bin').write_bytes(inputs);(work/'expected.bin').write_bytes(expected)
        report['corpus']=dict(cases=len(rows),distinct_inputs=len(set(rows)),groups=dict(collections.Counter(groups)),ones=sum(wanted),zeros=len(wanted)-sum(wanted),input_sha256=sha(inputs),output_sha256=sha(expected),metamorphic_pairs=len(pairs))
        (work/'corpus.json').write_text(json.dumps(report['corpus'],indent=2)+'\n');print('CORPUS',json.dumps(report['corpus']),flush=True)
        qemu,qid=build_qemu(work,source/'qemu_adapter.S',image);omega,oid=build_omega(work,lab/'seed-omc',(source/'ranges.om').read_text())
        report['qemu_identity']=qid;report['omega_identity']=oid
        report['qemu']=run_command(qemu,inputs,expected);print('QEMU_PASS',report['qemu']['cases'],flush=True)
        report['omega']=run_command(omega,inputs,expected);print('OMEGA_PASS',report['omega']['cases'],flush=True)
        report['unicorn']=run_unicorn(image,rows,expected,work)
        report['status']='THREE_WAY_DIFFERENTIAL_PASS'
    except Exception as ex:
        report['status']='FAIL';report['failure']=repr(ex);raise
    finally:(work/'stress-result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
