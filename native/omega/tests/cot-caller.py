import pathlib,subprocess,sys,tempfile,shutil,json,re,hashlib
w=pathlib.Path(sys.argv[1]).resolve();cc=sys.argv[2];phase=sys.argv[3];assert phase in ['red','green'];rows=[]
for broken in ([True] if phase=='red' else [False,True]):
 d=pathlib.Path(tempfile.mkdtemp(prefix='cot-caller-',dir=w/'build'));a=subprocess.run(['git','-C',str(w),'archive','HEAD','native/omega','native/kernel/gpu','native/kernel/dev','native/linux/aien-gpu','evidence/own-gpu-driver/rung2/window1_sim_test.sh'],capture_output=True,check=True);subprocess.run(['tar','-x','-C',str(d)],input=a.stdout,check=True)
 for f in ['native/linux/aien-gpu/Makefile','native/omega/cotplan.om','native/omega/tests/cotplan.py','native/omega/tests/cot-oracle.c','native/omega/tests/cot-io.py']:shutil.copyfile(w/f,d/f)
 if broken:(d/'native/omega/cotplan.om').write_text('fn main to int\nret 2\nend\n')
 p=subprocess.run(['make','-C',str(d/'native/linux/aien-gpu'),'sim-test','OMEGA_CC='+cc],capture_output=True,text=True,timeout=150);log=p.stdout+p.stderr;(d/'caller.log').write_text(log);objects=[]
 for m in re.finditer(r'(?m)^\{',p.stdout):
  try:objects.append(json.JSONDecoder().raw_decode(p.stdout[m.start():])[0])
  except ValueError:pass
 r=next((x for x in objects if 'builder_differentials' in x),{})
 ok=(p.returncode!=0 and r.get('failures',0)>0 and 'BOOT_NATIVE_GATE_PASS' not in log) if broken else (p.returncode==0 and r.get('failures')==0 and r.get('cases')==48 and log.count('WINDOW1_SIM_TEST: PASS')==2)
 rows.append({'broken_encoder':broken,'pass':ok,'exit':p.returncode,'encoder_cases':r.get('cases'),'encoder_failures':r.get('failures'),'simulation_passes':log.count('WINDOW1_SIM_TEST: PASS'),'log_sha256':hashlib.sha256(log.encode()).hexdigest()})
print(json.dumps({'cases':len(rows),'failures':sum(not x['pass'] for x in rows),'phase':phase,'makefile_sha256':hashlib.sha256((w/'native/linux/aien-gpu/Makefile').read_bytes()).hexdigest(),'results':rows},indent=2));sys.exit(any(not x['pass'] for x in rows))
