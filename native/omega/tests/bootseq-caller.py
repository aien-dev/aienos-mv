import pathlib,subprocess,sys,tempfile,shutil,os,json,hashlib,re
w=pathlib.Path(sys.argv[1]).resolve();cc=pathlib.Path(sys.argv[2]).resolve();phase=sys.argv[3]
assert phase in ['red','green']
d=pathlib.Path(tempfile.mkdtemp(prefix='boot-caller-',dir=w/'build'));base=d/'base';base.mkdir()
paths=['native/omega','native/kernel/gpu','native/kernel/dev','native/linux/aien-gpu','evidence/own-gpu-driver/rung2/window1_sim_test.sh']
a=subprocess.run(['git','-C',str(w),'archive','HEAD',*paths],capture_output=True,check=True)
subprocess.run(['tar','-x','-C',str(base)],input=a.stdout,check=True)
make=w/'native/linux/aien-gpu/Makefile';shutil.copyfile(make,base/'native/linux/aien-gpu/Makefile')
rows=[]
cases=[('broken-native',True,False)] if phase=='red' else [('qualified-native',False,False),('broken-native',True,False),('optimized-broken-native',True,True),('cached-success-then-broken',True,False)]
for name,broken,opt in cases:
 c=d/name;shutil.copytree(base,c)
 if name=='cached-success-then-broken':
  before=subprocess.run(['make','-C',str(c/'native/linux/aien-gpu'),'sim-test','OMEGA_CC='+str(cc)],capture_output=True,text=True,timeout=120)
  assert before.returncode==0 and 'BOOT_NATIVE_GATE_PASS' in before.stdout
  (c/'native/linux/aien-gpu/omega-boot-test').write_text('stale target file must not bypass PHONY gate\n')
 if broken:(c/'native/omega/bootseq.om').write_text('fn main to int\nret 2\nend\n')
 env=os.environ.copy()
 if opt:env['PYTHONOPTIMIZE']='1'
 p=subprocess.run(['make','-C',str(c/'native/linux/aien-gpu'),'sim-test','OMEGA_CC='+str(cc)],capture_output=True,text=True,env=env,timeout=120)
 log=p.stdout+p.stderr;(d/(name+'.log')).write_text(log)
 objects=[]
 for match in re.finditer(r'(?m)^\{',p.stdout):
  try:objects.append(json.JSONDecoder().raw_decode(p.stdout[match.start():])[0])
  except ValueError:pass
 native=next((x for x in objects if x.get('cases')==125),{})
 if broken:ok=p.returncode!=0 and 'omega-boot-test' in log and 'BOOT_NATIVE_GATE_PASS' not in log and native.get('failures',0)>0
 else:ok=p.returncode==0 and 'BOOT_NATIVE_GATE_PASS' in log and log.count('WINDOW1_SIM_TEST: PASS')==2 and native.get('failures')==0
 rows.append({'case':name,'pass':ok,'exit':p.returncode,'native_cases':native.get('cases'),'native_failures':native.get('failures'),'boot_gate_pass':'BOOT_NATIVE_GATE_PASS' in log,'simulation_passes':log.count('WINDOW1_SIM_TEST: PASS'),'log_sha256':hashlib.sha256(log.encode()).hexdigest()})
if phase=='green':
 c=d/'changed-compiler';shutil.copytree(base,c)
 changed=d/'changed-omc';changed.write_bytes(cc.read_bytes()+b'pin-negative');changed.chmod(0o700)
 env=os.environ.copy();env['PYTHONOPTIMIZE']='1'
 p=subprocess.run(['make','-C',str(c/'native/linux/aien-gpu'),'omega-boot-test','OMEGA_CC='+str(changed)],capture_output=True,text=True,env=env,timeout=120)
 log=p.stdout+p.stderr;(d/'changed-compiler.log').write_text(log)
 rows.append({'case':'optimized-changed-compiler','pass':p.returncode!=0 and 'AssertionError' in log and '01b1545b23e8ff40' in log and 'BOOT_NATIVE_GATE_PASS' not in log,'exit':p.returncode,'log_sha256':hashlib.sha256(log.encode()).hexdigest()})
print(json.dumps({'phase':phase,'cases':len(rows),'failures':sum(not x['pass'] for x in rows),'makefile_sha256':hashlib.sha256(make.read_bytes()).hexdigest(),'results':rows},indent=2));sys.exit(any(not x['pass'] for x in rows))
