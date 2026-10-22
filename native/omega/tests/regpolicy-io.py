import pathlib,subprocess,struct,os,json,sys
w=pathlib.Path(sys.argv[1]).resolve();exe=w/'build/omega-regpolicy/regpolicy';packet=b'RGP1'+struct.pack('<7I',1,0,2,1,8653824,1,8653824);rows=[]
p=subprocess.run([str(exe)],input=packet,capture_output=True);assert p.returncode==0 and p.stdout==struct.pack('<4I',1,2,0,114),'positive two-doorbell control'
p=subprocess.run(['sh','-c','exec "$1" 0<&-','sh',str(exe)],capture_output=True);rows.append({'case':'closed-input','pass':p.returncode==2 and not p.stdout,'exit':p.returncode})
r,wr=os.pipe();os.set_blocking(r,False)
try:
 os.write(wr,packet);p=subprocess.run([str(exe)],stdin=r,capture_output=True,timeout=5)
 rows.append({'case':'read-error-after-frame','pass':p.returncode==2 and not p.stdout,'exit':p.returncode})
finally:os.close(r);os.close(wr)
with open('/dev/full','wb',buffering=0) as full:
 p=subprocess.run([str(exe)],input=packet,stdout=full,stderr=subprocess.PIPE);rows.append({'case':'output-enospc','pass':p.returncode==2,'exit':p.returncode})
print(json.dumps({'cases':len(rows),'failures':sum(not x['pass'] for x in rows),'results':rows},indent=2));sys.exit(any(not x['pass'] for x in rows))
