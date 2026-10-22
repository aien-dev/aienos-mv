from pathlib import Path
import subprocess,re,hashlib,json
w=Path.home()/'workspace/aienos-omega-driver-direct';pin='f3a00467438e92bc6f5e5e2059ccbdad22859932';rows=[]
def body(s,name):
 b=re.search(r'(?ms)^fn '+name+r' .*?(?=^fn |\Z)',s).group();return '\n'.join(x for x in b.splitlines() if not x.startswith('note ')).strip()
for old,new,names in [('regpolicy.om','regcore.om',['init','canread','canwrite','allow']),('gspseq.om','gsq3.om',['size','poll','step','valid'])]:
 a=subprocess.check_output(['git','-C',str(w),'show',pin+':native/omega/'+old],text=True);b=(w/'native/omega'/new).read_text()
 for name in names:
  x,y=body(a,name),body(b,name);assert x==y,(new,name)
  rows.append({'origin':old,'consumer':new,'function':name,'sha256':hashlib.sha256(x.encode()).hexdigest()})
print(json.dumps({'origin_commit':pin,'unchanged_function_bodies':rows},indent=2))
