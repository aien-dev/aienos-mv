import pathlib,subprocess,sys,json,tempfile,shutil
w=pathlib.Path(sys.argv[1]).resolve();cc=sys.argv[2];s=(w/'native/omega/cotplan.om').read_text();rows=[]
files=['regcore.om','bootsim.om','bootsim-model.om','bootcore.om','tests/cotplan.py','tests/cot-oracle.c']
for name,old,new in [('swapped-fmc-address','copy 44 56 8','copy 12 56 8'),('wrong-size-field','copy 52 72 4','copy 52 76 4'),('truncated-signature','copy 56 88 816','copy 56 88 815'),('wrong-vendor','word 48 uint 336649854','word 48 uint 336649855')]:
 assert s.count(old)==1;d=pathlib.Path(tempfile.mkdtemp(prefix='cot-mutant-'+name+'-',dir=w/'build'))
 for f in files:
  p=d/'native/omega'/f;p.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(w/'native/omega'/f,p)
 for f in ['gb10_gsp_boot.c','gb10_gsp_boot.h']:
  p=d/'native/kernel/dev'/f;p.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(w/'native/kernel/dev'/f,p)
 (d/'native/omega/cotplan.om').write_text(s.replace(old,new))
 p=subprocess.run([sys.executable,str(d/'native/omega/tests/cotplan.py'),str(d),cc],capture_output=True,text=True);r=json.loads(p.stdout)
 assert p.returncode==1 and r['failures']>0,(name,p.stderr,r)
 rows.append({'mutant':name,'compiled':True,'caught':True,'failures':r['failures'],'cases':r['cases']})
print(json.dumps({'mutants':len(rows),'results':rows},indent=2))
