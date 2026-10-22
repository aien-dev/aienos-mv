import pathlib,subprocess,sys,json
w=pathlib.Path(sys.argv[1]).resolve();o=pathlib.Path(sys.argv[2]).resolve();p=w/'native/omega/gsq3.om';c=w/'native/omega/regcore.om';original={p:p.read_bytes(),c:c.read_bytes()};rows=[]
mutants=[('no-permission-preflight',p,b'guard = permit stop',b'guard = int 0'),('consume-shadow-latch',p,b'put C 1 latch',b'put C 1 get C 1'),('missing-modify-read',p,b'if or eq op uint 1 or eq op uint 2 eq op uint 4',b'if or eq op uint 2 eq op uint 4'),('aliased-registers',p,b'ret i\nend\nend\nif eq addr',b'ret ulong 0\nend\nend\nif eq addr'),('shared-repeat-doorbell',c,b'if ne get C 1 uint 0',b'if false')]
try:
 for name,path,old,new in mutants:
  for f,b in original.items():f.write_bytes(b)
  assert original[path].count(old)==1;path.write_bytes(original[path].replace(old,new))
  q=subprocess.run(['python3',str(w/'native/omega/tests/gsq3.py'),str(w),str(o)],capture_output=True,text=True,timeout=60);assert q.stdout,q.stderr
  r=json.loads(q.stdout);assert q.returncode==1 and r['failures']>0
  rows.append({'mutant':name,'compiled':True,'failures':r['failures']})
finally:
 for f,b in original.items():f.write_bytes(b)
print(json.dumps(rows,indent=2))
