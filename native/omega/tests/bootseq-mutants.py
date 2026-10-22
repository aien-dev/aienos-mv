import pathlib,subprocess,sys,json,tempfile,shutil
w=pathlib.Path(sys.argv[1]).resolve();cc=sys.argv[2];s=(w/'native/omega/bootseq.om').read_text();rows=[]
mutants=[('doorbell-order','wr uint 8653828 sub mul count uint 4 uint 4\nwr uint 8653824 uint 0','wr uint 8653824 uint 0\nwr uint 8653828 sub mul count uint 4 uint 4'),('skip-emem-readback','if ne sub emem get Meta 6 start count','if false'),('big-endian-packet','mul uint k 8','mul sub uint 3 uint k 8'),('extra-poll','if ge n budget','if gt n budget'),('ignore-high-pa','if eq get Meta 11 get Config 3','if true'),('ignore-dead-core','if eq and get Meta 12 uint 128 uint 0','if false')]
files=['native/omega/regcore.om','native/omega/tests/bootseq.py','native/omega/tests/bootseq-oracle.c']+['native/kernel/gpu/'+x for x in ['gsp_mmio.c','gsp_mmio.h','gmmu.h']]
for name,old,new in mutants:
 assert s.count(old)==1
 d=pathlib.Path(tempfile.mkdtemp(prefix='boot-mutant-'+name+'-',dir=w/'build'))
 for f in files:
  p=d/f;p.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(w/f,p)
 (d/'native/omega/bootseq.om').write_text(s.replace(old,new))
 p=subprocess.run([sys.executable,str(d/'native/omega/tests/bootseq.py'),str(d),cc],capture_output=True,text=True)
 result=json.loads(p.stdout) # Compile/setup failures are NOT killed mutants.
 assert p.returncode==1 and result['failures']>0,(name,p.stderr,result)
 rows.append({'mutant':name,'compiled':True,'caught':True,'failures':result['failures'],'cases':result['cases'],'source_sha256':result['source_sha256']})
print(json.dumps({'mutants':len(rows),'results':rows},indent=2))
