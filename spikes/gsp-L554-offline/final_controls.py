#!/usr/bin/env python3
"""Cross-oracle boundary checks and fail-closed pin controls, private copies only."""
from pathlib import Path
import hashlib,json,os,shutil,struct,subprocess,sys

def sha(b):return hashlib.sha256(b).hexdigest()

def main():
    src=Path(sys.argv[1]);lab=Path(sys.argv[2]);work=lab/'stress';snap=lab/'snapshot-20261022T212950Z'
    record={'status':'RUNNING','checks':[]}
    try:
        assert json.loads((work/'stress-result.json').read_text())['status']=='THREE_WAY_DIFFERENTIAL_PASS'
        assert json.loads((work/'adversarial-result.json').read_text())['status']=='ADVERSARIAL_PASS'
        wf=json.loads((work/'wolfram-boundaries.json').read_text());rows=wf['rows'];inputs=b''.join(struct.pack('<QQQQ',*r[:4]) for r in rows);expected=b''.join(struct.pack('<Q',r[4]) for r in rows)
        for name,cmd in [('QEMU',[str(work/'tools/usr/bin/qemu-riscv64'),str(work/'qemu-oracle.elf')]),('Omega',[str(work/'omega-stress.elf')])]:
            p=subprocess.run(cmd,input=inputs,capture_output=True,timeout=10);assert (p.returncode,p.stdout,p.stderr)==(0,expected,b'')
            record['checks'].append(dict(name='Wolfram-boundary-'+name,cases=len(rows),exit=p.returncode))
        # Negative binding controls run original published scripts unchanged.
        scratch=work/'pin-controls';scratch.mkdir(exist_ok=True);original=(snap/'gsp_ga10x.bin').read_bytes();copy=scratch/'gsp_ga10x.bin'
        py=lab/'emulator-venv/bin/python'
        for kind,content in [('truncated-image',original[:64]),('changed-image',original[:1024]+bytes([original[1024]^1])+original[1025:])]:
            copy.write_bytes(content);before=set(p.name for p in scratch.iterdir())
            for name in ['print_segments.py','emulate_firmware.py']:
                p=subprocess.run([str(py),str(src/name),str(scratch)],capture_output=True,text=True,timeout=20)
                assert p.returncode!=0 and 'AssertionError' in p.stderr and not p.stdout
                assert set(q.name for q in scratch.iterdir())==before
                record['checks'].append(dict(name=kind+'-'+name,exit=p.returncode,stderr=p.stderr,mutated_sha256=sha(content),no_output_files=True))
        # An executable-looking but wrong compiler is refused before execution.
        fake=scratch/'seed-omc';fake.write_text('#!/bin/sh\nprintf SHOULD_NOT_EXECUTE\n');fake.chmod(0o755)
        p=subprocess.run(['python3',str(src/'verify_ranges.py'),str(scratch),str(snap),str(src/'ranges.om')],capture_output=True,text=True,timeout=10)
        assert p.returncode!=0 and 'AssertionError' in p.stderr and 'SHOULD_NOT_EXECUTE' not in p.stdout
        record['checks'].append(dict(name='wrong-compiler-pin',exit=p.returncode,stderr=p.stderr))
        # Bind both oracle corpus and expected outputs before compiling/comparing.
        fake.write_bytes((lab/'seed-omc').read_bytes());fake.chmod(0o755)
        for f in ['leaf-input.bin','leaf-expected.bin','emulation.json']:shutil.copyfile(snap/f,scratch/f)
        for f in ['leaf-input.bin','leaf-expected.bin']:
            data=(scratch/f).read_bytes();(scratch/f).write_bytes(bytes([data[0]^1])+data[1:])
            p=subprocess.run(['python3',str(src/'verify_ranges.py'),str(scratch),str(scratch),str(src/'ranges.om')],capture_output=True,text=True,timeout=10)
            assert p.returncode!=0 and 'AssertionError' in p.stderr and not p.stdout
            assert not (scratch/'ranges-green.elf').exists()
            record['checks'].append(dict(name='changed-'+f,exit=p.returncode,stderr=p.stderr));(scratch/f).write_bytes(data)
        installed=Path('/lib/firmware/nvidia/580.173.02/gsp_ga10x.bin');manifest=json.loads((snap/'manifest.json').read_text());assert sha(installed.read_bytes())==sha(original)==manifest['sha256']
        record['firmware_unchanged_sha256']=manifest['sha256'];record['status']='FINAL_CONTROLS_PASS'
    except Exception as ex:
        record['status']='FAIL';record['failure']=repr(ex);raise
    finally:(work/'final-controls.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps({'status':record['status'],'checks':len(record['checks']),'boundary_cases_per_engine':len(rows),'firmware_unchanged':record['firmware_unchanged_sha256']},indent=2))

if __name__=='__main__':main()
