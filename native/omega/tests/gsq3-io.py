import pathlib,subprocess,struct,os,json,sys
w=pathlib.Path(sys.argv[1]).resolve();exe=w/'build/omega-gsq3/gsq3';v=[4,3,1,1,1,1,0]+list(range(100,114))+list(range(200,208))+[0,8653824,7,99];packet=b'GSQ3'+struct.pack('<'+'I'*len(v),*v);rows=[]
regs=list(range(100,114));regs[5]=7;expected=struct.pack('<28I',*([0,0,1,0,0,1]+regs+list(range(200,208))))
p=subprocess.run([str(exe)],input=packet,capture_output=True);assert p.returncode==0 and p.stdout==expected,'positive permitted-doorbell control'
p=subprocess.run(['sh','-c','exec "$1" 0<&-','sh',str(exe)],capture_output=True);rows.append({'case':'closed-input','pass':p.returncode==2 and not p.stdout,'exit':p.returncode})
r,wr=os.pipe();os.set_blocking(r,False)
try:
 os.write(wr,packet);p=subprocess.run([str(exe)],stdin=r,capture_output=True,timeout=5)
 rows.append({'case':'read-error-after-frame','pass':p.returncode==2 and not p.stdout,'exit':p.returncode})
finally:os.close(r);os.close(wr)
with open('/dev/full','wb',buffering=0) as full:
 p=subprocess.run([str(exe)],input=packet,stdout=full,stderr=subprocess.PIPE);rows.append({'case':'output-enospc','pass':p.returncode==2,'exit':p.returncode})
print(json.dumps({'cases':len(rows),'failures':sum(not x['pass'] for x in rows),'results':rows},indent=2));sys.exit(any(not x['pass'] for x in rows))
