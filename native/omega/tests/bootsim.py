import pathlib,subprocess,sys,struct,hashlib,json,random,argparse
ap=argparse.ArgumentParser();ap.add_argument('root');ap.add_argument('compiler');ap.add_argument('--old-tape',action='store_true');ap.add_argument('--model');ap.add_argument('--out');args=ap.parse_args()
w=pathlib.Path(args.root).resolve();cc=pathlib.Path(args.compiler).resolve();d=pathlib.Path(args.out).resolve() if args.out else w/'build/omega-bootsim';d.mkdir(parents=True,exist_ok=True)
assert hashlib.sha256(cc.read_bytes()).hexdigest()=='01b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6'
pins={'gsp_mmio.c':'6a4f26064c5a55985cf60a62c50e1e84c18c8559fad7e7c81602b6fcff78f9c7','gsp_mmio.h':'70511e0c2311c0af445b1254d79be8e48663e9472a8afdc670a705c7434067cd','gmmu.h':'fc5d44f61bbe28e6174fd390d27ca6db58bc5d294a32b13fc2ec2c9a9e6c7437','gsp_sim.c':'de5c9f59f95ef2d34bd4e95f25dc8f369a4a0afbd255589c9cfa52d0fbdf6282','gsp_sim.h':'444b7535bb56b109b4247e62a4defb3140462aa56925f688a76de8676372466a'}
for n,h in pins.items():assert hashlib.sha256((w/'native/kernel/gpu'/n).read_bytes()).hexdigest()==h
oracles=[]
for suffix,flags in [('plain',[]),('san',['-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
 exe=d/('oracle-'+suffix);subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',*flags,'-I'+str(w/'native/kernel/gpu'),str(w/'native/omega/tests/bootsim-oracle.c'),str(w/'native/kernel/gpu/gsp_mmio.c'),str(w/'native/kernel/gpu/gsp_sim.c'),'-o',str(exe)],check=True);oracles.append(exe)
files=[w/'native/omega'/n for n in (['regcore.om','bootseq.om','bootcore.om'] if args.old_tape else ['regcore.om','bootsim.om','bootsim-model.om','bootcore.om'])]
if args.model:assert not args.old_tape;files[2]=pathlib.Path(args.model)
s=b'\n'.join(f.read_bytes() for f in files);p=subprocess.run([str(cc)],input=s,capture_output=True);assert p.returncode==0,p.stderr
assert len(p.stdout)>=64 and p.stdout[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<HH',p.stdout,16)==(2,183)
ph=struct.unpack_from('<Q',p.stdout,32)[0];ps,pn=struct.unpack_from('<HH',p.stdout,54);assert ps==56 and pn and ph+ps*pn<=len(p.stdout);assert not {2,3}.intersection(struct.unpack_from('<I',p.stdout,ph+i*ps)[0] for i in range(pn))
exe=d/'bootsim';exe.write_bytes(p.stdout);exe.chmod(0o700)
rng=random.Random(332616);good=bytearray(rng.randbytes(868));struct.pack_into('<III',good,0,0xc0000000,0x1410de7e,(860<<16)|1);cases=[]
def frame(blob,mc=None,budget=8,lo=0x12345678,hi=0x40,pause=1):return b'BTS1'+struct.pack('<10I',len(blob),0,lo,hi,0xffffffff,*([budget]*4),pause)+blob+struct.pack('<9I',*(mc or [0]*9))
def add(name,blob=good,mc=None,budget=8,stage=None,**kw):cases.append((name,frame(blob,mc,budget,**kw),stage))
add('happy',stage=0)
for n in [0,1,2,3,4,7,8,11,12,867,868,869,1024,1025,2048]:add('length-'+str(n),good if n==868 else rng.randbytes(n),stage=1 if n==0 or n>1024 else (0 if n==868 else 5))
for index,stage in [(0,2),(1,4),(2,5),(3,6),(4,6),(8,5)]:
 for budget in [0,1,4,128]:
  mc=[0]*9;mc[index]=0xffffffff;add(f'stuck-{index}-b{budget}',mc=mc,budget=budget,stage=stage)
for index,stage in [(5,7),(6,8),(7,3)]:
 mc=[0]*9;mc[index]=0xdead if index==5 else 1;add('fault-'+str(index),mc=mc,stage=stage)
for index in [0,1,2,3,4,8]:
 for delay in [1,2,8,128]:
  mc=[0]*9;mc[index]=delay;add(f'delay-{index}-{delay}',mc=mc,budget=128)
for offset in [0,4,8,10]:
 blob=bytearray(good);blob[offset]^=1;add('bad-header-'+str(offset),blob,stage=5)
add('zero-low-pa',lo=0,stage=6);add('max-pa',lo=0xffffffff,hi=0xffffffff,stage=0)
add('no-pause',mc=[2,2,2,2,2,0,0,0,2],pause=0)
add('all-delayed',mc=[128,128,128,0,128,0,0,0,0],budget=128)
for i in range(64):
 mc=[rng.randrange(7) for _ in range(9)];mc[5]=0xdead if i%11==0 else 0;mc[6]=int(i%13==0);mc[7]=int(i%17==0)
 add('random-'+str(i),mc=mc,budget=rng.choice([0,1,4,16,128]),pause=i%2)
rows=[];stages=set()
for name,data,stage in cases:
 outs=[]
 for oracle in oracles:
  p=subprocess.run([str(oracle)],input=data,capture_output=True,timeout=10);assert p.returncode==0,(name,p.stderr);outs.append(p.stdout)
 assert outs[0]==outs[1];expected=outs[0];actualstage=struct.unpack_from('<I',expected)[0];stages.add(actualstage)
 if stage is not None:assert actualstage==stage,(name,actualstage,stage)
 p=subprocess.run([str(exe)],input=data,capture_output=True,timeout=10)
 (d/(name+'.input')).write_bytes(data);(d/(name+'.expected')).write_bytes(expected)
 rows.append({'case':name,'pass':p.returncode==0 and p.stdout==expected,'exit':p.returncode,'stage':actualstage,'expected_sha256':hashlib.sha256(expected).hexdigest(),'actual_sha256':hashlib.sha256(p.stdout).hexdigest(),'expected_bytes':len(expected)})
assert stages==set(range(9))
for name,data in [('magic',b'BTP1'+cases[0][1][4:]),('short',b'BTS1'),('extra',cases[0][1]+b'x'),('capacity',b'x'*2129),('budget',frame(good,budget=129)),('pause',frame(good,pause=2)),('flag',frame(good,[0,0,0,0,0,0,2,0,0]))]:
 p=subprocess.run([str(exe)],input=data,capture_output=True);rows.append({'case':'bad-'+name,'pass':p.returncode==2 and not p.stdout,'exit':p.returncode})
print(json.dumps({'cases':len(rows),'differentials':len(cases),'failures':sum(not x['pass'] for x in rows),'stages':sorted(stages),'source_sha256':hashlib.sha256(s).hexdigest(),'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'oracle_pins':pins,'results':rows},indent=2));sys.exit(any(not x['pass'] for x in rows))
