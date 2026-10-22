import pathlib,subprocess,json,sys
w=pathlib.Path(sys.argv[1]).resolve();cc=pathlib.Path(sys.argv[2]).resolve();p=w/'native/omega/regtrace.om';source=p.read_bytes();rows=[];out=w/'build/omega-regtrace/regtrace'
mutants=[('bypass-policy',b'if ne allow op addr uint 0',b'if false'),('skip-coverage',b'if ne get Seen 0 uint 63',b'if false'),('no-trace-reset',b'put C 1 uint 0\nput Pos',b'put C 1 get C 1\nput Pos'),('empty-time',b'if eq k 0',b'if false')]
try:
 for name,old,new in mutants:
  assert source.count(old)==1;p.write_bytes(source.replace(old,new));r=subprocess.run(['python3',str(w/'native/omega/tests/regtrace.py'),str(w),str(cc),str(out)],capture_output=True,text=True);assert r.stdout,r.stderr
  d=json.loads(r.stdout);assert r.returncode==1 and d['failures']>0;rows.append({'mutant':name,'compiled':True,'failures':d['failures']})
finally:p.write_bytes(source)
print(json.dumps(rows,indent=2))
