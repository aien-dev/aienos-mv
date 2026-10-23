import pathlib,subprocess,sys,json,tempfile
w=pathlib.Path(sys.argv[1]).resolve();cc=sys.argv[2];source=(w/'native/omega/bootsim-model.om').read_text();rows=[]
mutants=[('no-emem-increment','if eq get MC 7 uint 0','if false'),('accept-bad-vendor','if ne get Mem 1 uint 336649854','if false'),('ignore-zero-mask-delay','if and eq get Model 0 uint 2 ne get Model 2 uint 0','if false'),('ignore-dead-core','if eq get MC 6 uint 0','if true'),('skip-argument-delay','if eq get MC 3 uint 0','if true'),('wrong-lockdown-clear','put Model 8 and get Model 8 uint 4294959103','put Model 8 get Model 8')]
for name,old,new in mutants:
 assert source.count(old)==1
 d=pathlib.Path(tempfile.mkdtemp(prefix='bootsim-mutant-'+name+'-',dir=w/'build'));m=d/'model.om';m.write_text(source.replace(old,new))
 p=subprocess.run([sys.executable,str(w/'native/omega/tests/bootsim.py'),str(w),cc,'--model',str(m),'--out',str(d/'out')],capture_output=True,text=True)
 r=json.loads(p.stdout);assert p.returncode==1 and r['failures']>0,(name,p.stderr,r)
 rows.append({'mutant':name,'compiled':True,'caught':True,'failures':r['failures'],'cases':r['cases'],'source_sha256':r['source_sha256']})
print(json.dumps({'mutants':len(rows),'results':rows},indent=2))
