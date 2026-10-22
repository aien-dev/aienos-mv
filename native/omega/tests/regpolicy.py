import pathlib,subprocess,sys,struct,random,json,hashlib,re,errno
w=pathlib.Path(sys.argv[1]).resolve();o=pathlib.Path(sys.argv[2]).resolve();d=w/'build/omega-regpolicy';d.mkdir(parents=True,exist_ok=True)
cc=o/'seed/out/omc';assert hashlib.sha256(cc.read_bytes()).hexdigest()=='01b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6'
shim=(w/'native/linux/aien-gpu/aien_gpu.c').read_bytes();assert hashlib.sha256(shim).hexdigest()=='10990a78ff93af70f8a7c2f36fb92fa05d8a53f787f70df77dfd73f28db6d5d6'
sets={}
for name in ['rd','wr']:
 body=re.search(r'aien_gpu_'+name+r'_ok\[\] = \{(.*?)\};',shim.decode(),re.S).group(1)
 sets[name]=set(int(x,16) for x in re.findall(r'0x[0-9a-fA-F]+',body))
bell=int(re.search(r'#define AIEN_GPU_SEC2_DOORBELL (0x[0-9a-fA-F]+)',shim.decode()).group(1),16)
source=b'\n'.join((w/'native/omega'/name).read_bytes() for name in ['regcore.om','regpolicy.om']);p=subprocess.run([str(cc)],input=source,capture_output=True);assert p.returncode==0,p.stderr
assert p.stdout[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<HH',p.stdout,16)==(2,183)
phoff=struct.unpack_from('<Q',p.stdout,32)[0];ps,pn=struct.unpack_from('<HH',p.stdout,54);assert ps==56 and pn and phoff+ps*pn<=len(p.stdout)
assert not {2,3}.intersection(struct.unpack_from('<I',p.stdout,phoff+i*ps)[0] for i in range(pn))
exe=d/'regpolicy';exe.write_bytes(p.stdout);exe.chmod(0o700)
def frame(boot,latch,requests):return b'RGP1'+struct.pack('<III',boot,latch,len(requests))+b''.join(struct.pack('<II',*r) for r in requests)
def expected(boot,latch,req):
 statuses=[]
 for op,off in req:
  if op==0:status=0 if off in sets['rd'] else errno.EPERM
  elif op!=1:status=errno.ENOTTY
  elif not boot or off not in sets['wr']:status=errno.EPERM
  elif off==bell and latch:status=errno.EALREADY
  else:
   status=0
   if off==bell:latch+=1
  statuses.append(status)
 return struct.pack('<'+'I'*(2+len(statuses)),latch,len(statuses),*statuses)
rng=random.Random(332);offsets=set.union(*sets.values())
for off in list(offsets):offsets.update((off+x)&0xffffffff for x in [-4,-1,1,4])
offsets.update(rng.getrandbits(32) for _ in range(64));offsets=sorted(offsets);assert len(offsets)+2<=256
cases=[]
for boot in [0,1]:
 for latch in [0,1,0xffffffff]:
  for op in [0,1,2,0xffffffff]:
   req=[(op,x) for x in offsets]+[(op,bell)]*2
   cases.append((f'op{op}-boot{boot}-latch{latch}',frame(boot,latch,req),0,expected(boot,latch,req),req,boot,latch))
for name,boot,latch,req in [('empty',1,0,[]),('mixed',1,0,[(1,bell),(0,bell),(1,bell),(1,next(x for x in sorted(sets['wr']) if x!=bell))]),('capacity',1,0,[(0,0)]*256)]:
 cases.append((name,frame(boot,latch,req),0,expected(boot,latch,req),req,boot,latch))
for name,data in [('short',b'RGP1'),('wrong-version',b'RGP0'+frame(1,0,[])[4:]),('boot-range',frame(2,0,[])),('extra-byte',frame(1,0,[])+b'x'),('over-capacity',frame(1,0,[(0,0)]*257))]:cases.append((name,data,2,b'',[],0,0))
rows=[];coverage={'rd':set(),'wr':set()};total=0
for name,data,status,out,req,boot,latch in cases:
 p=subprocess.run([str(exe)],input=data,capture_output=True,timeout=10);total+=len(req)
 rows.append({'case':name,'pass':(p.returncode,p.stdout)==(status,out),'exit':p.returncode,'expected_exit':status,'requests':len(req),'actual_sha256':hashlib.sha256(p.stdout).hexdigest(),'expected_sha256':hashlib.sha256(out).hexdigest()})
 if status==0 and p.returncode==0 and len(p.stdout)==len(out):
  values=struct.unpack('<'+'I'*(len(p.stdout)//4),p.stdout)[2:]
  for (op,off),v in zip(req,values):
   if op in [0,1] and v==0:coverage['rd' if op==0 else 'wr'].add(off)
# Equality in both directions: neither undeclared accepts nor missing allowed addresses.
coverage_ok=coverage==sets
print(json.dumps({'cases':len(rows),'failures':sum(not x['pass'] for x in rows),'requests':total,'allowlist_equality':coverage_ok,'read_allowed':sorted(coverage['rd']),'write_allowed':sorted(coverage['wr']),'source_sha256':hashlib.sha256(source).hexdigest(),'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'shim_sha256':hashlib.sha256(shim).hexdigest(),'results':rows},indent=2));sys.exit(any(not x['pass'] for x in rows) or not coverage_ok)
