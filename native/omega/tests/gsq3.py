import pathlib,subprocess,struct,hashlib,json,sys,re,random
w=pathlib.Path(sys.argv[1]).resolve();o=pathlib.Path(sys.argv[2]).resolve();d=w/'build/omega-gsq3';d.mkdir(parents=True,exist_ok=True)
cc=o/'seed/out/omc';assert hashlib.sha256(cc.read_bytes()).hexdigest()=='01b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6'
shim=(w/'native/linux/aien-gpu/aien_gpu.c').read_bytes();assert hashlib.sha256(shim).hexdigest()=='10990a78ff93af70f8a7c2f36fb92fa05d8a53f787f70df77dfd73f28db6d5d6'
lists={n:[int(x,16) for x in re.findall(r'0x[0-9a-fA-F]+',re.search('aien_gpu_'+n+r'_ok\[\] = \{(.*?)\};',shim.decode(),re.S).group(1))] for n in ['rd','wr']}
addresses=lists['rd']+[x for x in lists['wr'] if x not in lists['rd']];assert len(addresses)==14
bell=0x840c00;R=set(lists['rd']);W=set(lists['wr']);arity=[2,3,5,1,2,0,0,0,0]
parts=[(w/'native/omega'/n).read_bytes() for n in ['regcore.om','gsq3.om']];source=b'\n'.join(parts)
p=subprocess.run([str(cc)],input=source,capture_output=True);assert p.returncode==0,p.stderr
assert p.stdout[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<HH',p.stdout,16)==(2,183)
ph=struct.unpack_from('<Q',p.stdout,32)[0];ps,pn=struct.unpack_from('<HH',p.stdout,54);assert ps==56 and pn and ph+ps*pn<=len(p.stdout);assert not {2,3}.intersection(struct.unpack_from('<I',p.stdout,ph+i*ps)[0] for i in range(pn))
exe=d/'gsq3';exe.write_bytes(p.stdout);exe.chmod(0o700)
def model(words,stop,budget,core,delay,boot,latch,regs,save):
 original=[0,0,0,0,0,latch]+regs+save;ops=[];i=0
 if not words or stop>=len(words):return [1]+original[1:]
 while i<stop:
  op=words[i];i+=1
  if op>8 or i+arity[op]>stop:return [1]+original[1:]
  a=words[i:i+arity[op]];i+=arity[op]
  if (op==4 and a[1]>=8) or (op>=5 and core==0):return [1]+original[1:]
  ops.append((op,a))
 shadow=latch
 for op,a in ops:
  if op in (1,2,4) and a[0] not in R:return [3]+original[1:]
  if op in (0,1):
   if not boot or a[0] not in W or (a[0]==bell and shadow):return [3]+original[1:]
   if a[0]==bell:shadow=1
 counters=[0]*5;rr=dict(zip(addresses,regs));ss=save[:]
 for op,a in ops:
  if op==0:
   counters[2]+=1;rr[a[0]]=a[1]
   if a[0]==bell:latch=1
  elif op==1:
   counters[1]+=1;counters[2]+=1;rr[a[0]]=(rr[a[0]]&~a[1])|a[2]
   if a[0]==bell:latch=1
  elif op==2:
   good=rr[a[0]]&a[1]==a[2];counters[1]+=1 if good else 1+max(budget,1)
   if not good:counters[0]=1;break
  elif op==3:
   if delay:counters[3]=(counters[3]+a[0])&0xffffffff
  elif op==4:counters[1]+=1;ss[a[1]]=rr[a[0]]
  else:
   counters[4]+=1
   if core==2:counters[0]=1;break
 return counters+[latch]+[rr[x] for x in addresses]+ss
cases=[]
def add(name,program,boot=1,latch=0,budget=1,core=1,delay=1,stop=None,raw=False):
 words=program if raw else program+[99];stop=len(program) if stop is None else stop
 regs=list(range(100,114));save=list(range(200,208));values=[len(words),stop,budget,core,delay,boot,latch]+regs+save+words
 data=b'GSQ3'+struct.pack('<'+'I'*len(values),*values);expected=model(words,stop,budget,core,delay,boot,latch,regs,save)
 cases.append((name,data,expected[0],struct.pack('<28I',*expected)))
for off in sorted(R|W|{0xffffffff,1,0x840c01,8}):
 for op,body in [(0,[off,255]),(1,[off,15,16]),(2,[off,0,0,0,0]),(4,[off,7])]:
  add(f'op{op}-addr{off}',[op]+body)
  add(f'prefix-op{op}-addr{off}',[0,0x840ac0,777,op]+body,boot=0)
for latch in [0,1,0xffffffff]:
 for boot in [0,1]:
  add(f'double-bell-{boot}-{latch}',[0,bell,7,0,bell,8],boot=boot,latch=latch)
  add(f'single-bell-{boot}-{latch}',[0,bell,7],boot=boot,latch=latch)
add('write-modify-poll-save',[0,0x840ac0,255,1,0x840ac0,15,16,2,0x840ac0,0xffffffff,240,0,0,4,0x840ac0,0])
add('read-consumed-latch',[4,bell,7],boot=0,latch=0xffffffff)
add('modify-doorbell',[1,bell,15,16])
add('late-denied-preserves-prefix',[0,0x840ac0,777,4,0xffffffff,0])
add('timeout-does-not-consume-future-bell',[2,0,0xffffffff,999,0,0,0,bell,7])
add('prior-bell-survives-timeout',[0,bell,7,2,0,0xffffffff,999,0,0])
add('modify-writeonly-denied',[1,0x840ac4,0,7])
add('structural-precedes-policy',[0,0xffffffff,7,99])
add('bad-save',[4,0,8]);add('truncated',[0,0]);add('zero-words',[],raw=True,stop=0)
add('empty-selected-range',[99],stop=0,raw=True)
for mode in [0,1,2]:
 for op in range(5,9):add(f'core{op}-{mode}',[op],core=mode)
for budget in [0,1,4,1024]:add(f'poll-budget{budget}',[2,0,0xffffffff,999,0,0],budget=budget)
for delay in [0,1]:add(f'delay-wrap-{delay}',[3,0xffffffff,3,2],delay=delay)
add('maximum-words',[3,1]*511+[5])
base=cases[0][1]
for name,data in [('old-version',b'GSQ2'+base[4:]),('short',base[:119]),('trailing',base+b'x'),('over-capacity',b'x'*4217),('boot-range',base[:24]+struct.pack('<I',2)+base[28:]),('core-range',base[:16]+struct.pack('<I',3)+base[20:])]:cases.append((name,data,2,b''))
rows=[]
for name,data,status,out in cases:
 p=subprocess.run([str(exe)],input=data,capture_output=True,timeout=10)
 rows.append({'case':name,'pass':(p.returncode,p.stdout)==(status,out),'exit':p.returncode,'expected_exit':status,'actual':p.stdout.hex(),'expected':out.hex()})
print(json.dumps({'cases':len(rows),'failures':sum(not r['pass'] for r in rows),'source_sha256':hashlib.sha256(source).hexdigest(),'part_sha256':[hashlib.sha256(x).hexdigest() for x in parts],'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'results':rows},indent=2));sys.exit(any(not r['pass'] for r in rows))
