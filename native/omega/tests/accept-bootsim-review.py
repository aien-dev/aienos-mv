from pathlib import Path
import subprocess,hashlib,json
w=Path.home()/'workspace/aienos-omega-driver-direct';e=w/'evidence/omega-sequencer';d=w/'build/omega-bootsim'
pin='5697de6e201499cd5e1db8717109c0bb317bfe66'
subprocess.run(['git','-C',str(w),'diff','--exit-code',pin,'--','native',':(exclude)native/omega/tests/accept-bootsim-review.py'],check=True)
receipt=json.loads((e/'bootsim-green.json').read_text())
for names,expected in [(['regcore.om','bootsim.om','bootsim-model.om','bootcore.om'],'65e14024e2488140941917b2b89f20be64a7a4205f55086efd121c008114b2ba'),(['regcore.om','bootseq.om','bootcore.om'],'480b93638b2dfc30517527b52a75eeba2c07d1d3730c5bf965e77b93fbac8b2d')]:
 assert hashlib.sha256(b'\n'.join((w/'native/omega'/n).read_bytes() for n in names)).hexdigest()==expected
assert hashlib.sha256((w/'native/omega/bootcore.om').read_bytes()).hexdigest()=='7e156753aa407ff8c572e81e6d18396998fcb796c377fa92a0aa31db5c8930a8'
exe=d/'bootsim';assert hashlib.sha256(exe.read_bytes()).hexdigest()==receipt['binary_sha256']
rows=[];byname={r['case']:r for r in receipt['results']}
for name in ['happy','fault-5','fault-6','fault-7','bad-header-4','zero-low-pa']:
 data=(d/(name+'.input')).read_bytes();expected=(d/(name+'.expected')).read_bytes();assert hashlib.sha256(expected).hexdigest()==byname[name]['expected_sha256']
 p=subprocess.run([str(exe)],input=data,capture_output=True,timeout=10)
 rows.append({'case':name,'pass':p.returncode==0 and p.stdout==expected,'exit':p.returncode,'input_sha256':hashlib.sha256(data).hexdigest(),'output_sha256':hashlib.sha256(p.stdout).hexdigest(),'expected_sha256':byname[name]['expected_sha256']})
assert len(rows)==6 and all(r['pass'] for r in rows)
print(json.dumps({'reviewed_source':pin,'published_candidate':'8fc965a23b80cdb713c0c5dc7812adee10bc0a50','reviewed_source_unchanged':True,'binary_sha256':receipt['binary_sha256'],'cases':len(rows),'results':rows},indent=2))
