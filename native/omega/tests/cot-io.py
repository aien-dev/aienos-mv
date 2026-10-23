import pathlib,subprocess,sys,os,json
w=pathlib.Path(sys.argv[1]).resolve();d=w/'build/omega-cot';exe=str(d/'cotplan');data=(d/'0.input').read_bytes();expected=(d/'0.expected').read_bytes();rows=[]
p=subprocess.run([exe],input=data,capture_output=True,timeout=5);assert p.returncode==0 and p.stdout==expected
p=subprocess.run([exe],stdout=subprocess.PIPE,stderr=subprocess.PIPE,preexec_fn=lambda:os.close(0),timeout=5);rows.append({'case':'closed-input','pass':p.returncode==2 and not p.stdout})
r,wr=os.pipe();os.set_blocking(r,False);assert os.write(wr,data)==len(data)
p=subprocess.run([exe],stdin=r,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=5);os.close(r);os.close(wr);rows.append({'case':'EAGAIN-after-frame-before-EOF','pass':p.returncode==2 and not p.stdout})
with open('/dev/full','wb') as f:p=subprocess.run([exe],input=data,stdout=f,stderr=subprocess.PIPE,timeout=5)
rows.append({'case':'output-ENOSPC','pass':p.returncode==2})
print(json.dumps({'cases':len(rows),'results':rows},indent=2));assert all(x['pass'] for x in rows)
