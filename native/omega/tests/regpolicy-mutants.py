import pathlib,subprocess,sys,json
w=pathlib.Path(sys.argv[1]).resolve();o=pathlib.Path(sys.argv[2]).resolve();p=w/'native/omega/regpolicy.om';source=p.read_bytes();rows=[]
mutants=[('read-all',b'if eq off get R i',b'if true'),('ignore-boot-switch',b'if or eq get C 0 uint 0 not canwrite off',b'if not canwrite off'),('repeat-doorbell',b'if ne get C 1 uint 0',b'if false'),('wrong-read-register',b'put R 12 uint 1119112',b'put R 12 uint 1119113')]
try:
 for name,old,new in mutants:
  assert source.count(old)==1;p.write_bytes(source.replace(old,new))
  r=subprocess.run(['python3',str(w/'native/omega/tests/regpolicy.py'),str(w),str(o)],capture_output=True,text=True,timeout=60);assert r.stdout,r.stderr
  d=json.loads(r.stdout);assert r.returncode==1 and d['failures']>0
  rows.append({'mutant':name,'compiled':True,'failures':d['failures'],'allowlist_equality':d['allowlist_equality']})
finally:p.write_bytes(source)
print(json.dumps(rows,indent=2))
