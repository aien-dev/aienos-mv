import pathlib,subprocess,sys,os,json,hashlib
w=pathlib.Path(sys.argv[1]).resolve();cc=pathlib.Path(sys.argv[2]).resolve();d=w/'build/omega-regtrace';d.mkdir(parents=True,exist_ok=True)
assert hashlib.sha256(cc.read_bytes()).hexdigest()=='01b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6'
script=w/'evidence/own-gpu-driver/rung2/window1_sim_test.sh';sim=w/'native/linux/aien-gpu/build/aien_gsp_boot_sim';shim=w/'native/linux/aien-gpu/aien_gpu.c'
fake=d/'drain-success.sh';fake.write_text('#!/bin/sh\ncat >/dev/null\nexit 0\n');fake.chmod(0o700)
stub=b'global Tmp uchar 4096\nfn main to int\nwhile true\nr = read 0 Tmp 0 len Tmp\nif lt r 0\nret 2\nend\nif eq r 0\nret 0\nend\nend\nret 0\nend\n'
p=subprocess.run([str(cc)],input=stub,capture_output=True);assert p.returncode==0 and p.stdout[:4]==b'\x7fELF',p.stderr
wrong=d/'wrong-native';wrong.write_bytes(p.stdout);wrong.chmod(0o700)
rows=[]
for name,checker,want in [('qualified-native',d/'regtrace',0),('draining-shell-noop',fake,2),('draining-native-noop',wrong,2),('optimized-native-noop',wrong,2),('changed-compiler',d/'regtrace',2)]:
 before=checker.read_bytes();p=subprocess.run(['bash',str(script),str(sim),str(shim),str(checker)],env=dict(os.environ,OMEGA_CC=str(wrong if name=='changed-compiler' else cc),PYTHONOPTIMIZE='1' if name=='optimized-native-noop' else '0'),capture_output=True,text=True,timeout=40)
 rows.append({'case':name,'exit':p.returncode,'expected_exit':want,'pass':p.returncode==want and checker.read_bytes()==before,'reported_pass':'WINDOW1_SIM_TEST: PASS' in p.stdout})
print(json.dumps({'cases':len(rows),'failures':sum(not x['pass'] for x in rows),'results':rows},indent=2));sys.exit(any(not x['pass'] for x in rows))
