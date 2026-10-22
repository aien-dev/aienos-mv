import pathlib,subprocess,struct,os,json,sys
w=pathlib.Path(sys.argv[1]).resolve();exe=w/'build/omega-sequencer/gspseq';packet=struct.pack('<22I',1,0,4,0,1,*range(8),*([0]*8),0);rows=[]
p=subprocess.run(['sh','-c','exec "$1" 0<&-','sh',str(exe)],capture_output=True);rows.append({'case':'closed-input','pass':p.returncode==2 and not p.stdout,'exit':p.returncode})
r,wr=os.pipe();os.set_blocking(r,False)
try:
 os.write(wr,packet)
 p=subprocess.run([str(exe)],stdin=r,capture_output=True,timeout=5)
 rows.append({'case':'read-error-after-complete-frame','pass':p.returncode==2 and not p.stdout,'exit':p.returncode})
finally:os.close(r);os.close(wr)
with open('/dev/full','wb',buffering=0) as full:
 p=subprocess.run([str(exe)],input=packet,stdout=full,stderr=subprocess.PIPE)
 rows.append({'case':'output-enospc','pass':p.returncode==2,'exit':p.returncode})
print(json.dumps({'cases':len(rows),'failures':sum(not x['pass'] for x in rows),'results':rows},indent=2));sys.exit(any(not x['pass'] for x in rows))
