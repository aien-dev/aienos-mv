#!/usr/bin/env python3
"""Adversarial tests for the published research tools and Omega transport.
Private corpus mutations only; no installed firmware changes or GPU access.
"""
from pathlib import Path
import collections,hashlib,importlib.util,json,os,random,signal,struct,subprocess,sys,threading,time
import unicorn
from unicorn import riscv_const as rv

MASK=(1<<64)-1

def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod);return mod

def main():
    src=Path(sys.argv[1]);lab=Path(sys.argv[2]);work=lab/'stress';snap=lab/'snapshot-20261022T212950Z'
    printer=load('gsp_print',src/'print_segments.py');em=load('gsp_emu',src/'emulate_firmware.py');stress=load('gsp_stress',src/'stress_ranges.py')
    image=(snap/'fwimage.bin').read_bytes();mapping=json.loads((snap/'binary-map.json').read_text());report={'status':'RUNNING'}
    try:
        count=0;rejected=0;valid=0
        def trial(data,want,name):
            nonlocal count,rejected,valid
            count+=1
            try:got=printer.candidate(data,0)
            except (AssertionError,struct.error):
                assert not want,('unexpected rejection',name);rejected+=1;return
            assert want,('unexpected acceptance',name,got);valid+=1
        for obj in mapping['objects']:
            data=image[obj['offset']:obj['offset']+obj['extent']];trial(data,True,'original')
            h=struct.unpack_from(printer.EH,data);cuts=set(range(65))|{h[5]+k for k in range(h[9]*h[10])}|{len(data)-1}
            for cut in sorted(cuts):trial(memoryview(data)[:cut],False,'truncate-'+str(cut))
            # These fields are advisory/discarded in the packed format, not load data.
            b=bytearray(data);struct.pack_into('<Q',b,40,MASK);trial(b,True,'ignored-stale-section-offset')
            b=bytearray(data);b[9]^=0x80;trial(b,True,'ignored-ident-padding')
        data=image[mapping['objects'][0]['offset']:mapping['objects'][0]['offset']+mapping['objects'][0]['extent']]
        h=struct.unpack_from(printer.EH,data);phoff=h[5];idx=next(i for i in range(h[10]) if struct.unpack_from('<II',data,phoff+i*56)==(1,5));p=phoff+idx*56;seg=struct.unpack_from(printer.PH,data,p)
        fields=[('magic',0,'B',0),('class',4,'B',1),('endian',5,'B',2),('ident-version',6,'B',2),('type',16,'H',1),('machine',18,'H',183),('version',20,'I',0),('ehsize',52,'H',0),('phentsize',54,'H',0),('phnum-zero',56,'H',0),('phnum-large',56,'H',33),('phoff-out',32,'Q',len(data)-1),('entry-out',24,'Q',0),('exec-removed',p+4,'I',4),('filesz-out',p+32,'Q',len(data)+1),('memsz-short',p+40,'Q',seg[5]-1),('va-wrap',p+16,'Q',MASK),('align-nonpower',p+48,'Q',3),('align-mismatch',p+16,'Q',seg[3]+1)]
        for name,off,fmt,val in fields:
            b=bytearray(data);struct.pack_into('<'+fmt,b,off,val);trial(b,False,name)
        b=bytearray(data);struct.pack_into('<Q',b,p+40,seg[5]+4096);struct.pack_into('<Q',b,24,seg[3]+seg[5]+8);trial(b,False,'entry-only-bss')
        rng=random.Random(55491);fuzz=collections.Counter()
        for _ in range(10000):
            b=bytearray(data)
            for __ in range(rng.randrange(1,5)):
                ix=rng.randrange(phoff+h[9]*h[10]);b[ix]^=1<<rng.randrange(8)
            try:
                obj=printer.candidate(b,0)
            except (AssertionError,struct.error):fuzz['refused']+=1
            else:
                # A bad accepted result must FAIL this harness, not be counted
                # as a parser refusal by the exception handler above.
                assert obj['extent']<=len(b) and any(s['type']==1 and s['flags']&1 and s['vaddr']<=obj['entry']<s['vaddr']+s['filesz'] for s in obj['segments'])
                fuzz['structurally_accepted']+=1
        assert sum(fuzz.values())==10000
        report['parser']=dict(directed=count,expected_rejections=rejected,expected_acceptances=valid,random_mutations=dict(fuzz))
        # Deliberate privileged/system and missing-memory probes must not continue.
        controls=[]
        fixtures=[('ecall',0x73),('ebreak',0x100073),('mret',0x30200073),('mstatus-csr',(0x300<<20)|(2<<12)|(10<<7)|0x73),('satp-csr',(0x180<<20)|(2<<12)|(10<<7)|0x73)]
        for name,word in fixtures:
            got=em.boot(struct.pack('<I',word));assert got['stop']['kind']=='SYSTEM_INSTRUCTION_BOUNDARY' and got['instructions_observed']==1
            controls.append(dict(name=name,stop=got['stop'],observed=got['instructions_observed']))
        for name,code,kind in [('unmapped-read',struct.pack('<I',(3<<12)|(10<<7)|3),'UNMAPPED_MEMORY'),('unmapped-jump',struct.pack('<I',0x67),'UNMAPPED_MEMORY'),('infinite-loop',struct.pack('<I',0x6f),'INSTRUCTION_LIMIT'),('page-padding-read',struct.pack('<II',(5<<7)|0x17,(8<<20)|(5<<15)|(3<<12)|(10<<7)|3),'OUTSIDE_IMAGE_BYTES')]:
            got=em.boot(code);assert got['stop'].get('kind')==kind,(name,got)
            controls.append(dict(name=name,stop=got['stop'],observed=got['instructions_observed']))
        got=em.boot(b'\x02\x90');assert got['instructions_observed']==1 and 'emulator_error' in got['stop'];controls.append(dict(name='compressed-break',stop=got['stop'],observed=1))
        report['emulator_controls']=controls
        # Unspecified-register poison and every feasible branch path, not just a0.
        u=em.machine(image[:4096]);u.mem_protect(em.BASE,4096,unicorn.UC_PROT_READ|unicorn.UC_PROT_EXEC);u.mem_map(em.STOP,4096,unicorn.UC_PROT_READ|unicorn.UC_PROT_EXEC)
        trace=[];paths=collections.Counter()
        def hook(uc,pc,size,_):
            assert em.BASE+0x28<=pc<em.BASE+0x54,'escaped leaf'
            trace.append(hex(pc-em.BASE))
        u.hook_add(unicorn.UC_HOOK_CODE,hook);rng=random.Random(55492);rows=list(struct.iter_unpack('<QQQQ',(snap/'leaf-input.bin').read_bytes()));saved=[2,3,4,8,9,*range(18,28)]
        for args in rows:
            before={i:rng.getrandbits(64) for i in range(1,32)}
            for i,v in before.items():u.reg_write(getattr(rv,'UC_RISCV_REG_X'+str(i)),v)
            for reg,v in zip([rv.UC_RISCV_REG_A0,rv.UC_RISCV_REG_A1,rv.UC_RISCV_REG_A2,rv.UC_RISCV_REG_A3],args):u.reg_write(reg,v)
            u.reg_write(rv.UC_RISCV_REG_RA,em.STOP);trace.clear();u.emu_start(em.BASE+0x28,em.STOP,count=64)
            assert u.reg_read(rv.UC_RISCV_REG_PC)==em.STOP and u.reg_read(rv.UC_RISCV_REG_A0)==stress.oracle(*args)
            for i in saved:assert u.reg_read(getattr(rv,'UC_RISCV_REG_X'+str(i)))==before[i],i
            paths[tuple(trace)]+=1
        proof=json.loads((work/'symbolic-proof.json').read_text());assert set(paths)=={tuple(p['trace']) for p in proof['path_details'] if p['feasible']}
        report['register_poison']=dict(cases=len(rows),preserved_registers=saved,paths=[dict(trace=list(p),cases=n) for p,n in paths.items()])
        body=(src/'ranges.om').read_text();cmd,identity=stress.build_omega(work,lab/'seed-omc',body,'omega-io')
        old=(snap/'leaf-input.bin').read_bytes();want=(snap/'leaf-expected.bin').read_bytes();io_rows=[]
        # Exact 1..31-byte truncations at 0, 1 and 17 completed record boundaries.
        for records in [0,1,17]:
            for n in range(1,32):
                p=subprocess.run(cmd,input=old[:records*32]+old[:n],capture_output=True,timeout=5)
                assert (p.returncode,p.stdout,p.stderr)==(2,want[:records*8],b'')
                io_rows.append(dict(test='truncation',records=records,tail=n,exit=p.returncode))
        for fd in [0,1]:
            p=subprocess.run(cmd,input=old[:32],capture_output=True,preexec_fn=lambda fd=fd:os.close(fd),timeout=5)
            assert p.returncode==3 and p.stdout==b'' and p.stderr==b'';io_rows.append(dict(test='closed-fd',fd=fd,exit=p.returncode))
        with open('/dev/full','wb',buffering=0) as sink:
            p=subprocess.run(cmd,input=old[:32],stdout=sink,stderr=subprocess.PIPE,timeout=5);assert p.returncode==3 and not p.stderr
            io_rows.append(dict(test='output-ENOSPC',exit=p.returncode))
        for fragment in [1,2,3,7,31]:
            payload=old[:32*64];expected=want[:8*64];p=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0);errors=[]
            def writer():
                try:
                    for k in range(0,len(payload),fragment):
                        piece=payload[k:k+fragment];sent=0
                        while sent<len(piece):sent+=os.write(p.stdin.fileno(),piece[sent:])
                    p.stdin.close()
                except Exception as ex:errors.append(repr(ex))
            t=threading.Thread(target=writer);t.start();output=p.stdout.read();err=p.stderr.read();rc=p.wait(timeout=5);t.join(timeout=5)
            assert not t.is_alive() and not errors and (rc,output,err)==(0,expected,b'')
            io_rows.append(dict(test='fragmented-input',fragment=fragment,records=64,exit=rc))
        report['transport']=io_rows
        cap=4096*512;limits=[]
        for count,tail,rc in [(999999,b'',0),(1000000,b'',0),(1000001,b'',0),(cap-1,b'',0),(cap,b'',0),(cap,bytes(1),4),(cap,bytes(32),4)]:
            payload=bytes(32)*count+tail;p=subprocess.run(cmd,input=payload,capture_output=True,timeout=120)
            assert (p.returncode,p.stdout,p.stderr)==(rc,bytes(8)*count,b''),(count,len(tail),p.returncode,len(p.stdout))
            limits.append(dict(records=count,extra_bytes=len(tail),exit=p.returncode,output_bytes=len(p.stdout)))
        report['capacity']=dict(records=cap,cases=limits)
        muts=[('no-zero','if or eq n 0 eq m 0\nret 0\nend\n',''),('first-overflow','if gt n sub ulong 18446744073709551615 a\nret 1','if gt n sub ulong 18446744073709551615 a\nret 0'),('second-overflow','if gt m sub ulong 18446744073709551615 b\nret 1','if gt m sub ulong 18446744073709551615 b\nret 0'),('inclusive-a','if and lt a add b m lt b add a n','if and le a add b m lt b add a n'),('inclusive-b','if and lt a add b m lt b add a n','if and lt a add b m le b add a n'),('signed-order','if and lt a add b m lt b add a n','if and signcmp a add b m signcmp b add a n'),('false-overlap','if and lt a add b m lt b add a n\nret 1','if and lt a add b m lt b add a n\nret 0'),('empty-returns-one','if or eq n 0 eq m 0\nret 0','if or eq n 0 eq m 0\nret 1')]
        mutations=[];report['mutants']=mutations
        for name,a,b in muts:
            assert body.count(a)==1;changed=body.replace(a,b);assert changed!=body
            if name=='signed-order':
                changed=changed.replace('fn ranges','fn signcmp a ulong b ulong to bool\nret lt xor a 9223372036854775808 xor b 9223372036854775808\nend\n\nfn ranges',1)
            c,ident=stress.build_omega(work,lab/'seed-omc',changed,'stress-mut-'+name);p=subprocess.run(c,input=old,capture_output=True,timeout=10);assert p.returncode==0 and len(p.stdout)==len(want) and not p.stderr
            mismatches=sum(x!=y for x,y in zip(struct.iter_unpack('<Q',p.stdout),struct.iter_unpack('<Q',want)));assert mismatches>0,name
            mutations.append(dict(name=name,compiled_exit=ident['compile_rc'],run_exit=p.returncode,mismatches=mismatches,source_sha256=ident['source_sha256'],binary_sha256=ident['executable_sha256']))
        report['mutants']=mutations;stress.run_command(cmd,old,want);report['status']='ADVERSARIAL_PASS'
    except Exception as ex:
        report['status']='FAIL';report['failure']=repr(ex);raise
    finally:(work/'adversarial-result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
