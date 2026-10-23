import pathlib,sys,subprocess,hashlib,struct,json,random
w=pathlib.Path(sys.argv[1]).resolve();cc=pathlib.Path(sys.argv[2]).resolve();d=w/'build/omega-cot';d.mkdir(parents=True,exist_ok=True)
assert hashlib.sha256(cc.read_bytes()).hexdigest()=='01b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6'
for n,h in [('gb10_gsp_boot.c','1d5db0d3933b8dec333bd26547c10aa4edb97b9ed73b66a6656d5ddcbc9a491f'),('gb10_gsp_boot.h','9315898a9e2a187a22c0a6891b0d379c8f35f8dbc5ab92a8c1df495e239e7eca')]:assert hashlib.sha256((w/'native/kernel/dev'/n).read_bytes()).hexdigest()==h
oracles=[]
for name,flags in [('plain',[]),('san',['-fsanitize=address,undefined','-fno-sanitize-recover=all'])]:
 out=d/name;subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror',*flags,'-I'+str(w/'native/kernel/dev'),str(w/'native/omega/tests/cot-oracle.c'),str(w/'native/kernel/dev/gb10_gsp_boot.c'),'-o',str(out)],check=True);oracles.append(out)
def compile_native(source,out):
 p=subprocess.run([str(cc)],input=source,capture_output=True);assert p.returncode==0,p.stderr
 assert len(p.stdout)>=64 and p.stdout[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<HH',p.stdout,16)==(2,183)
 ph=struct.unpack_from('<Q',p.stdout,32)[0];ps,pn=struct.unpack_from('<HH',p.stdout,54);assert ps==56 and ph+ps*pn<=len(p.stdout) and pn
 assert not {2,3}.intersection(struct.unpack_from('<I',p.stdout,ph+i*ps)[0] for i in range(pn))
 out.write_bytes(p.stdout);out.chmod(0o700)
s=(w/'native/omega/cotplan.om').read_bytes();exe=d/'cotplan';compile_native(s,exe)
bs=b'\n'.join((w/'native/omega'/n).read_bytes() for n in ['regcore.om','bootsim.om','bootsim-model.om','bootcore.om']);assert hashlib.sha256(bs).hexdigest()=='65e14024e2488140941917b2b89f20be64a7a4205f55086efd121c008114b2ba';sim=d/'bootsim';compile_native(bs,sim)
rows=[];rng=random.Random(332618)
for i in range(40):
 pa=[0x10000000,0x100000000,0xfffffffff0000000][i%3]+(i//3)*4096
 size=[1,3,255,256,4095,4096,4097,8192][i%8];opaque=rng.randbytes(816);data=struct.pack('<QI',pa,size)+opaque;out=[]
 for oracle in oracles:
  p=subprocess.run([str(oracle)],input=data,capture_output=True,timeout=10);assert p.returncode==0,p.stderr;assert len(p.stdout)==884;out.append(p.stdout)
 assert out[0]==out[1];fmc,args=struct.unpack_from('<QQ',out[0]);packet=out[0][16:]
 mc=[0]*9
 if i%5==1:mc[7]=1
 if i%5==2:mc[6]=1
 if i%5==3:mc[5]=0xdead
 if i%5==4:mc[0]=0xffffffff
 header=struct.pack('<10I',868,0,args&0xffffffff,args>>32,rng.randrange(2**32),8,8,8,8,i%2);model=struct.pack('<9I',*mc)
 source=b'CTP1'+header+struct.pack('<QI',fmc,size)+opaque+model;expected=b'BTS1'+header+packet+model
 p=subprocess.run([str(exe)],input=source,capture_output=True,timeout=5);ok=p.returncode==0 and p.stdout==expected
 # Native producer -> native causal consumer; no host re-encoding between them.
 producer=subprocess.Popen([str(exe)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 consumer=subprocess.Popen([str(sim)],stdin=producer.stdout,stdout=subprocess.PIPE,stderr=subprocess.PIPE);producer.stdout.close()
 producer.communicate(source,timeout=5);actual,_=consumer.communicate(timeout=5)
 reference=subprocess.run([str(sim)],input=expected,capture_output=True,timeout=5);assert reference.returncode==0
 ok=ok and producer.returncode==consumer.returncode==0 and actual==reference.stdout
 (d/(str(i)+'.input')).write_bytes(source);(d/(str(i)+'.expected')).write_bytes(expected)
 rows.append({'case':i,'pass':ok,'exit':p.returncode,'frame_sha256':hashlib.sha256(p.stdout).hexdigest(),'expected_sha256':hashlib.sha256(expected).hexdigest(),'pipeline_stage':struct.unpack_from('<I',actual)[0] if len(actual)>=4 else None})
base=(d/'0.input').read_bytes()
for name,data in [('short',base[:-1]),('extra',base+b'x'),('magic',b'CTP0'+base[4:]),('zero-size',base[:52]+bytes(4)+base[56:]),('packet-length',base[:4]+struct.pack('<I',867)+base[8:]),('budget',base[:24]+struct.pack('<I',129)+base[28:]),('pause',base[:40]+struct.pack('<I',2)+base[44:]),('model-flag',base[:896]+struct.pack('<I',2)+base[900:])]:
 p=subprocess.run([str(exe)],input=data,capture_output=True,timeout=5);rows.append({'case':name,'pass':p.returncode==2 and not p.stdout,'exit':p.returncode})
print(json.dumps({'cases':len(rows),'failures':sum(not x['pass'] for x in rows),'builder_differentials':40,'native_pipelines':40,'source_sha256':hashlib.sha256(s).hexdigest(),'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'consumer_source_sha256':hashlib.sha256(bs).hexdigest(),'results':rows},indent=2));sys.exit(any(not x['pass'] for x in rows))
