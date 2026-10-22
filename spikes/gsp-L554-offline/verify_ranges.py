#!/usr/bin/env python3
"""Compare handwritten seed-native host code against emulated vendor bytes.
Python/Unicorn are test tools, never shipped computation/build dependencies.
"""
import hashlib,json,struct,subprocess,sys
from pathlib import Path
EXPECTED_COMPILER='01b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6'

def sha(b):return hashlib.sha256(b).hexdigest()

def main():
    lab=Path(sys.argv[1]);snap=Path(sys.argv[2]);src=Path(sys.argv[3]);compiler=lab/'seed-omc'
    assert sha(compiler.read_bytes())==EXPECTED_COMPILER
    body=src.read_text();inputs=(snap/'leaf-input.bin').read_bytes();want=(snap/'leaf-expected.bin').read_bytes()
    oracle=json.loads((snap/'emulation.json').read_text())['leaf']
    assert sha(inputs)==oracle['input_sha256'] and sha(want)==oracle['expected_sha256']
    values=list(struct.unpack('<'+'Q'*(len(want)//8),want));records=list(struct.iter_unpack('<QQQQ',inputs));assert len(records)==len(values)==oracle['cases']
    logs=[]
    def build(text,name):
        p=lab/(name+'.om');p.write_text(text)
        r=subprocess.run([str(compiler)],input=text.encode(),capture_output=True,timeout=30)
        assert r.returncode==0 and not r.stderr,(name,r.returncode,r.stderr)
        assert len(r.stdout)>64 and r.stdout[:4]==b'\x7fELF'
        assert struct.unpack_from('<HH',r.stdout,16)==(2,183),'not AArch64 EXEC'
        q=lab/(name+'.elf');q.write_bytes(r.stdout);q.chmod(0o755)
        return q,dict(source_sha256=sha(text.encode()),binary_sha256=sha(r.stdout),compile_rc=r.returncode)
    def execute(exe,data):return subprocess.run([str(exe)],input=data,capture_output=True,timeout=30)
    exe,identity=build(body,'ranges-green');r=execute(exe,inputs)
    assert r.returncode==0 and not r.stderr and r.stdout==want,(r.returncode,len(r.stdout),len(want))
    logs.append(dict(name='green-stream',cases=len(values),mismatches=0,run_rc=r.returncode,**identity))
    # Refusals and successful records before an incomplete final record.
    r=execute(exe,b'');assert (r.returncode,r.stdout,r.stderr)==(0,b'',b'')
    for n in range(1,32):
        r=execute(exe,inputs[:n]);assert (r.returncode,r.stdout,r.stderr)==(2,b'',b''),(n,r.returncode,r.stdout)
    r=execute(exe,inputs[:64]+inputs[:7]);assert (r.returncode,r.stdout,r.stderr)==(2,want[:16],b'')
    logs.append(dict(name='transport',empty=1,short_records=31,valid_then_short=1,result='PASS'))
    # Direct core entry tests, with no read/write adapter and no binary oracle call at runtime.
    chosen=[(0,1,0,1),(0,1,1,1),(1,1,0,1),(1,1,1,1),(0,0,0,1),(0,1,0,0),
            ((1<<64)-1,1,0,1),(0,1,(1<<64)-1,1),((1<<64)-2,1,(1<<64)-1,1),
            (1<<63,1,1<<63,1),(0,(1<<64)-1,(1<<64)-1,1),((1<<64)-1,0,0,1)]
    byargs=dict(zip(records,values))
    for i,args in enumerate(chosen):
        assert args in byargs
        code=body.split('fn word')[0]+f'fn main to int\nif ne ranges '+ ' '.join('ulong '+str(v) for v in args)+f' ulong {byargs[args]}\nret 7\nend\nret 0\nend\n'
        direct,di=build(code,'direct-'+str(i));r=execute(direct,b'');assert (r.returncode,r.stdout,r.stderr)==(0,b'',b'')
    logs.append(dict(name='direct-core',cases=len(chosen),result='PASS'))
    muts=[('no-zero-guard','if or eq n 0 eq m 0\nret 0\nend\n',''),
          ('first-overflow','if gt n sub ulong 18446744073709551615 a\nret 1','if gt n sub ulong 18446744073709551615 a\nret 0'),
          ('second-overflow','if gt m sub ulong 18446744073709551615 b\nret 1','if gt m sub ulong 18446744073709551615 b\nret 0'),
          ('adjacent-overlaps','if and lt a add b m lt b add a n','if and le a add b m le b add a n')]
    for name,old,new in muts:
        assert body.count(old)==1;mut=body.replace(old,new);assert mut!=body
        m,mi=build(mut,'mut-'+name);r=execute(m,inputs)
        assert r.returncode==0 and len(r.stdout)==len(want) and not r.stderr
        got=struct.unpack('<'+'Q'*len(values),r.stdout);mismatches=sum(a!=b for a,b in zip(got,values));assert mismatches>0,name
        logs.append(dict(name=name,mutation_detected=True,mismatches=mismatches,run_rc=r.returncode,**mi))
    r=execute(exe,inputs);assert r.returncode==0 and r.stdout==want and not r.stderr
    closure=subprocess.run(['readelf','-h','-lW','-d',str(exe)],capture_output=True,text=True,check=True).stdout
    assert 'INTERP' not in closure and 'DYNAMIC' not in closure and 'There is no dynamic section' in closure
    (lab/'ranges-readelf.txt').write_text('\n'.join(line.rstrip() for line in closure.splitlines())+'\n')
    result=dict(compiler_sha256=EXPECTED_COMPILER,compiler_source_pin='413f87c84e9b4233e4a56487dc6643b2139b5d0c',oracle=oracle,logs=logs,result='HOST_NATIVE_MIRROR_PASS; NOT_RISCV_FIRMWARE; NOT_GPU_FUNCTION_PARITY')
    (lab/'green.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
