import pathlib,subprocess,sys,struct,hashlib,json,random
w=pathlib.Path(sys.argv[1]).resolve();cc=pathlib.Path(sys.argv[2]).resolve();d=w/'build/omega-bootseq';d.mkdir(parents=True,exist_ok=True)
assert hashlib.sha256(cc.read_bytes()).hexdigest()=='01b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6'
for n,h in [('gsp_mmio.c','6a4f26064c5a55985cf60a62c50e1e84c18c8559fad7e7c81602b6fcff78f9c7'),('gsp_mmio.h','70511e0c2311c0af445b1254d79be8e48663e9472a8afdc670a705c7434067cd'),('gmmu.h','fc5d44f61bbe28e6174fd390d27ca6db58bc5d294a32b13fc2ec2c9a9e6c7437')]:assert hashlib.sha256((w/'native/kernel/gpu'/n).read_bytes()).hexdigest()==h
oracle=d/'oracle';subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(w/'native/kernel/gpu'),str(w/'native/omega/tests/bootseq-oracle.c'),str(w/'native/kernel/gpu/gsp_mmio.c'),'-o',str(oracle)],check=True)
s=b'\n'.join((w/'native/omega'/n).read_bytes() for n in ['regcore.om','bootseq.om']);p=subprocess.run([str(cc)],input=s,capture_output=True);assert p.returncode==0,p.stderr
assert len(p.stdout)>=64 and p.stdout[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<HH',p.stdout,16)==(2,183)
ph=struct.unpack_from('<Q',p.stdout,32)[0];ps,pn=struct.unpack_from('<HH',p.stdout,54);assert ps==56 and pn and ph+ps*pn<=len(p.stdout);assert not {2,3}.intersection(struct.unpack_from('<I',p.stdout,ph+i*ps)[0] for i in range(pn))
exe=d/'bootseq';exe.write_bytes(p.stdout);exe.chmod(0o700)
H,T,EMC,SM0,SM1,GM0,GM1,HW,CPU=0x840c00,0x840c04,0x840ac0,0x840040,0x840044,0x110040,0x110044,0x1100f4,0x111388
AINC,BUSY,LOCKED=1<<24,0xc001cafe,0xbadf41a7
rng=random.Random(332);cases=[]
def packet(n):return bytes(rng.randrange(256) for _ in range(n))
def frame(blob,reads,budget=2,pause=1,lo=0x12345678,hi=0x40,version=0xffffffff):
 c=[len(blob),len(reads),lo,hi,version]+[budget]*4+[pause];return b'BTP1'+struct.pack('<10I',*c)+blob+b''.join(struct.pack('<II',*x) for x in reads)
def add(name,blob,kind='happy',budget=2,pause=1,lo=0x12345678,hi=0x40):
 reads=[];stage=0;delay=budget if kind=='delayed-max' else (2 if kind=='delayed' else 0)
 if not blob or len(blob)>1024:stage=1
 elif kind=='busy':reads=[(H,1),(T,0)]*(budget+1);stage=2
 else:
  start=64 if kind=='nonzero-pointer' else (16383 if kind=='wrap-pointer' else 0)
  after=start if kind=='emem' else (start+(len(blob)+3)//4)%16384
  reads=[(H,1),(T,0)]*delay+[(H,0),(T,0),(EMC,AINC+start*4),(EMC,AINC+after*4)]
  if kind in ['emem','wrap-pointer']:stage=3
  elif kind=='command':reads += [(SM0,BUSY)]*(budget+1)+[(SM1,0xdeadbeef)];stage=4
  else:
   reads += [(SM0,BUSY)]*delay+[(SM0,0)]
   if kind in ['mask','zero-mask']:reads += [(HW,LOCKED if kind=='mask' else 0)]*(budget+1);stage=5
   else:
    reads += [(HW,LOCKED)]*delay+[(HW,1)]
    if kind=='args':reads += [(GM0,lo),(GM1,hi)]*(budget+1)+[(GM1,hi)];stage=6
    elif kind=='lockdown':reads += [(GM0,0),(HW,8192)]*(budget+1)+[(GM1,7)];stage=6
    elif kind=='fmc':reads += [(GM0,0xdead),(HW,8192),(GM0,0xdead)];stage=7
    else:
     reads += [(GM0,lo),(GM1,hi)]*delay
     if kind=='different-high':reads += [(GM0,lo),(GM1,hi+1),(HW,1)]
     else:reads += [(GM0,0),(HW,1)]
     reads += [(GM0,0),(CPU,0 if kind=='dead' else 128)];stage=8 if kind=='dead' else 0
 cases.append((name,frame(blob,reads,budget,pause,lo,hi),stage,True))
for n in [0,1,2,3,4,5,7,8,15,16,31,32,63,64,65,255,256,257,867,868,869,1023,1024,1025,2048]:add(f'length-{n}',packet(n))
for i in range(32):add(f'random-{i}',packet(rng.randrange(1,1025)),pause=i%2)
for budget in [0,1,4,128]:
 for kind in ['busy','command','mask','zero-mask','args','lockdown']:
  for pause in [0,1]:add(f'{kind}-b{budget}-p{pause}',packet(7),kind,budget,pause)
for kind in ['emem','fmc','dead','different-high','delayed']:add(kind,packet(868),kind)
add('delayed-max',packet(1024),'delayed-max',budget=128)
add('nonzero-pointer',packet(257),'nonzero-pointer')
add('wrap-pointer',packet(1),'wrap-pointer')
add('zero-low-pa',packet(5),lo=0,hi=0xffffffff)
add('high-low-pa',packet(5),lo=0xffffffff,hi=0xffffffff)
cases += [('read-tape-empty',frame(b'x',[]),9,False),('read-tape-wrong-address',frame(b'x',[(0,0)]),9,False)]
rows=[];stages=set()
for name,data,stage,diff in cases:
 a=subprocess.run([str(exe)],input=data,capture_output=True,timeout=10);actual=a.stdout
 if diff:
  b=subprocess.run([str(oracle)],input=data,capture_output=True,timeout=10);assert b.returncode==0,(name,b.stderr)
  assert struct.unpack_from('<I',b.stdout)[0]==stage,(name,b.stdout[:68].hex())
  expected=b.stdout;ok=a.returncode==0 and actual==expected;stages.add(stage)
 else:expected=struct.pack('<17I',stage,*([0]*16));ok=a.returncode==0 and actual==expected
 (d/(name+'.input')).write_bytes(data);(d/(name+'.expected')).write_bytes(expected)
 rows.append({'case':name,'pass':ok,'stage':stage,'differential':diff,'exit':a.returncode,'native_header':list(struct.unpack('<17I',actual[:68])) if len(actual)>=68 else [],'reference_header':list(struct.unpack('<17I',expected[:68])),'actual_sha256':hashlib.sha256(actual).hexdigest(),'expected_sha256':hashlib.sha256(expected).hexdigest()})
assert stages==set(range(9))
for name,data in [('old-magic',b'BTP0'+cases[1][1][4:]),('short',b'BTP1'),('extra',cases[1][1]+b'x'),('capacity',b'x'*10285),('budget-range',frame(b'x',[],budget=129)),('pause-range',frame(b'x',[],pause=2)),('read-count-range',frame(b'x',[(H,0)]*1025)),('packet-envelope-range',frame(packet(2049),[]))]:
 a=subprocess.run([str(exe)],input=data,capture_output=True);rows.append({'case':name,'pass':a.returncode==2 and not a.stdout,'exit':a.returncode})
print(json.dumps({'cases':len(rows),'failures':sum(not x['pass'] for x in rows),'differential_cases':sum(x.get('differential',False) for x in rows),'stages':sorted(stages),'source_sha256':hashlib.sha256(s).hexdigest(),'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'oracle_source_sha256':hashlib.sha256((w/'native/omega/tests/bootseq-oracle.c').read_bytes()).hexdigest(),'results':rows},indent=2));sys.exit(any(not x['pass'] for x in rows))
