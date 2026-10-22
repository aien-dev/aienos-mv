import pathlib,subprocess,sys,json
w=pathlib.Path(sys.argv[1]).resolve();omega=pathlib.Path(sys.argv[2]).resolve();p=w/'native/omega/gspseq.om';source=p.read_bytes();rows=[]
mutants=[('accept-invalid-save',b'if ge index uint 8\nret 1',b'if ge index uint 8\nret 0'),('wrong-modify-mask',b'xor get Words add p 1 uint 4294967295',b'get Words add p 1'),('accept-disabled-core',b'if eq get Config 3 uint 0\nret 1',b'if eq get Config 3 uint 0\nret 0'),('zero-poll-budget',b'left = uint 1',b'left = uint 0')]
try:
 for name,old,new in mutants:
  assert source.count(old)==1;p.write_bytes(source.replace(old,new))
  r=subprocess.run(['python3',str(w/'native/omega/tests/check.py'),str(w),str(omega)],capture_output=True,text=True,timeout=120)
  assert r.stdout,r.stderr;d=json.loads(r.stdout);assert r.returncode==1 and d['failures']>0,(name,r.stderr)
  rows.append({'mutant':name,'compiled':True,'failures':d['failures'],'failed_cases':[x['case'] for x in d['results'] if not x['pass']]})
finally:p.write_bytes(source)
print(json.dumps(rows,indent=2))
