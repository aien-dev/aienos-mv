import pathlib,subprocess,json,sys
w=pathlib.Path(sys.argv[1]).resolve();d=w/'build/omega-regtrace';d.mkdir(parents=True,exist_ok=True);shim=w/'native/linux/aien-gpu/aien_gpu.c';sim=w/'native/linux/aien-gpu/build/aien_gsp_boot_sim';script=w/'evidence/own-gpu-driver/rung2/window1_sim_test.sh'
baseline=d/'baseline.sh';baseline.write_bytes(subprocess.check_output(['git','-C',str(w),'show','826c1c51f08524931a49b54dd5a60d6907b89253:evidence/own-gpu-driver/rung2/window1_sim_test.sh']))
wrong=d/'wrong-shim.c';wrong.write_bytes(shim.read_bytes()+b'\n');rows=[]
for name,s,sh,validator,want in [('old-ignores-native',baseline,shim,'/usr/bin/false',0),('new-refuses-native-failure',script,shim,'/usr/bin/false',1),('changed-shim-refused',script,wrong,str(d/'regtrace'),2)]:
 p=subprocess.run(['bash',str(s),str(sim),str(sh),str(validator)],capture_output=True,text=True,timeout=40);(d/(name+'.log')).write_text(p.stdout+p.stderr)
 assert p.returncode==want,(name,p.returncode,p.stdout,p.stderr)
 if want==1:assert 'Omega trace policy refused' in p.stdout and 'WINDOW1_SIM_TEST: FAIL' in p.stdout
 rows.append({'case':name,'exit':p.returncode,'pass':True})
tools=d/'blocked-host';tools.mkdir(exist_ok=True);log=d/'denied.log'
for name in ['sed','sort','comm']:
 f=tools/name;f.write_text('#!/bin/sh\nprintf "%s\\n" "'+name+'" >> "$DENIED_LOG"\nexit 97\n');f.chmod(0o700)
import os
env=dict(os.environ,PATH=str(tools)+':'+os.environ['PATH'],DENIED_LOG=str(log))
for name,s,want in [('old-needs-host-allowlist',baseline,1),('native-no-host-allowlist',script,0)]:
 log.write_text('');p=subprocess.run(['bash',str(s),str(sim),str(shim),str(d/'regtrace')],env=env,capture_output=True,text=True,timeout=40)
 assert p.returncode==want,(name,p.stdout,p.stderr)
 denied=log.read_text().splitlines();assert bool(denied)==(want==1)
 rows.append({'case':name,'exit':p.returncode,'denied_tools':denied,'pass':True})
print(json.dumps({'cases':len(rows),'results':rows},indent=2))
