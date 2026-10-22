import pathlib,subprocess,sys,json
w=pathlib.Path(sys.argv[1]).resolve();omega=pathlib.Path(sys.argv[2]).resolve();p=w/'native/omega/gspseq.om';source=p.read_bytes();rows=[]
mutants=[('skip-preflight',b'guard = valid n stop',b'guard = int 0'),('preflight-save-missing',b'if ge get Words add i 1 uint 8',b'if false'),('preflight-core-missing',b'if ge op uint 5',b'if false'),('wrong-modify-mask',b'xor get Words add p 1 uint 4294967295',b'get Words add p 1'),('zero-poll-budget',b'left = uint 1',b'left = uint 0')]
try:
 for name,old,new in mutants:
  assert source.count(old)==1;p.write_bytes(source.replace(old,new))
  r=subprocess.run(['python3',str(w/'native/omega/tests/check.py'),str(w),str(omega)],capture_output=True,text=True,timeout=120)
  assert r.stdout,r.stderr;d=json.loads(r.stdout);assert r.returncode==1 and d['failures']>0,(name,r.stderr)
  rows.append({'mutant':name,'compiled':True,'failures':d['failures'],'failed_cases':[x['case'] for x in d['results'] if not x['pass']]})
finally:p.write_bytes(source)
print(json.dumps(rows,indent=2))
