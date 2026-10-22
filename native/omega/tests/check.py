import pathlib,subprocess,sys,struct,random,json,hashlib,os
w=pathlib.Path(sys.argv[1]).resolve();omega=pathlib.Path(sys.argv[2]).resolve();d=w/'build/omega-sequencer';d.mkdir(parents=True,exist_ok=True)
cc=omega/'seed/out/omc';assert hashlib.sha256(cc.read_bytes()).hexdigest()=='01b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6'
for f,h in {'gsp_seq.c':'4c172bed0b18ee15d59b81c597126ced94eab177fcffb44dbbcca735a6baa193','gsp_seq.h':'fa46c1d5a3464805bfa2f03a4400315e2f40ae5fc426c95d1899bad95432cdea'}.items():
 assert hashlib.sha256((w/'native/kernel/gpu'/f).read_bytes()).hexdigest()==h,'reference source drift: '+f
source=(w/'native/omega/gspseq.om').read_bytes();source=b'fn main to int\nret 2\nend\n' if '--stub' in sys.argv else source
p=subprocess.run([str(cc)],input=source,capture_output=True);assert p.returncode==0,p.stderr
assert p.stdout[:4]==b'\x7fELF' and int.from_bytes(p.stdout[18:20],'little')==183
assert p.stdout[4:6]==b'\x02\x01' and struct.unpack_from('<H',p.stdout,16)[0]==2
phoff=struct.unpack_from('<Q',p.stdout,32)[0];phsize,phnum=struct.unpack_from('<HH',p.stdout,54)
assert phsize==56 and phnum>0 and phoff+phsize*phnum<=len(p.stdout)
assert not {2,3}.intersection(struct.unpack_from('<I',p.stdout,phoff+i*phsize)[0] for i in range(phnum))
exe=d/'gspseq';exe.write_bytes(p.stdout);exe.chmod(0o700)
subprocess.run(['cc','-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(w/'native/kernel/gpu'),str(w/'native/omega/tests/oracle.c'),str(w/'native/kernel/gpu/gsp_seq.c'),'-o',str(d/'oracle')],check=True)
def frame(words,end=None,budget=4,core=0,delay=1,regs=None,saved=None):
 data=[len(words),len(words)-1 if end is None else end,budget,core,delay]+(regs or list(range(8)))+(saved or [0]*8)+words
 return struct.pack('<'+'I'*len(data),*data)
fixtures=[]
def add(name,words,**kw):fixtures.append((name,frame(words,**kw)))
add('empty',[],end=0);add('empty-execution',[0],end=0)
for op,payload in [(0,[1,0xabc]),(1,[2,15,5]),(2,[3,255,3,10,1]),(3,[25]),(4,[4,7]),(5,[]),(6,[]),(7,[]),(8,[])]:
 for mode in range(3):add('op-'+str(op)+'-core-'+str(mode),[op]+payload+[0],core=mode)
 for cut in range(1,len(payload)+1):add('cut-'+str(op)+'-'+str(cut),[op]+payload+[0],end=cut)
add('index-equals-size',[0,1,2,0],end=4);add('index-huge',[0],end=4294967295)
add('save-eight',[4,4,8,0]);add('save-huge',[4,4,4294967295,0]);add('unknown',[99,0])
add('partial-effects',[0,1,99,4,1,0,99,0]);add('modify-mask',[1,2,15,16,0],regs=[0,0,255,0,0,0,0,0])
add('delay-disabled',[3,25,0],delay=0);add('delay-wrap',[3,4294967295,3,2,0])
for budget in [0,1,4,1024]:add('stuck-'+str(budget),[2,3,1,0,0,0,0],budget=budget)
add('write-poll-store',[0,3,7,2,3,255,7,10,1,4,3,0,0])
add('out-of-map',[0,99,123,4,99,0,0]);add('max-words',[3,1]*511+[0,0],end=1022)
fixtures += [('short',b'x'),('trailing',frame([0])+b'x'),('capacity',bytes(4181)),('budget-over',frame([0],budget=1025)),('core-over',frame([0],core=3)),('delay-over',frame([0],delay=2))]
rng=random.Random(332)
for k in range(150):
 words=[]
 for j in range(rng.randrange(1,12)):
  op=rng.randrange(10);words.append(op)
  if op==0:words += [rng.randrange(10),rng.getrandbits(32)]
  elif op==1:words += [rng.randrange(10),rng.getrandbits(32),rng.getrandbits(32)]
  elif op==2:words += [rng.randrange(10),rng.randrange(4),rng.randrange(4),rng.getrandbits(32),rng.getrandbits(32)]
  elif op==3:words += [rng.getrandbits(32)]
  elif op==4:words += [rng.randrange(10),rng.randrange(10)]
 words.append(0);add('random-'+str(k),words,core=rng.randrange(3),budget=rng.randrange(5),regs=[rng.getrandbits(32) for _ in range(8)],saved=[rng.getrandbits(32) for _ in range(8)])
rows=[]
for name,data in fixtures:
 a=subprocess.run([str(d/'oracle')],input=data,capture_output=True,timeout=10);assert a.returncode in [0,1,2] and not a.stderr,(name,a.returncode,a.stderr)
 b=subprocess.run([str(exe)],input=data,capture_output=True,timeout=10)
 rows.append({'case':name,'pass':(a.returncode,a.stdout)==(b.returncode,b.stdout),'expected_exit':a.returncode,'exit':b.returncode,'expected':a.stdout.hex(),'actual':b.stdout.hex()})
# Independent state invariants, not merely agreement with the C reference.
for name,data in fixtures:
 if name=='modify-mask':
  a=subprocess.run([str(d/'oracle')],input=data,capture_output=True);assert struct.unpack('<21I',a.stdout)[7]==240
 if name=='stuck-0':
  a=subprocess.run([str(d/'oracle')],input=data,capture_output=True);assert struct.unpack('<21I',a.stdout)[:2]==(1,2)
print(json.dumps({'cases':len(rows),'failures':sum(not r['pass'] for r in rows),'source_sha256':hashlib.sha256(source).hexdigest(),'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'oracle_source_sha256':hashlib.sha256((w/'native/kernel/gpu/gsp_seq.c').read_bytes()).hexdigest(),'results':rows},indent=2));sys.exit(any(not r['pass'] for r in rows))
