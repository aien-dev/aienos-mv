import pathlib,subprocess,sys,os,fcntl,json
w=pathlib.Path(sys.argv[1]).resolve();d=w/'build/omega-bootseq';exe=str(d/'bootseq');data=(d/'length-1024.input').read_bytes();expected=(d/'length-1024.expected').read_bytes();rows=[]
p=subprocess.run([exe],input=data,capture_output=True,timeout=5);assert p.returncode==0 and p.stdout==expected
p=subprocess.run([exe],stdout=subprocess.PIPE,stderr=subprocess.PIPE,preexec_fn=lambda:os.close(0),timeout=5);rows.append({'case':'closed-input','pass':p.returncode==2 and not p.stdout})
r,wr=os.pipe();os.set_blocking(r,False);assert os.write(wr,data)==len(data)
p=subprocess.run([exe],stdin=r,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=5);os.close(r);os.close(wr);rows.append({'case':'EAGAIN-after-frame-before-EOF','pass':p.returncode==2 and not p.stdout})
with open('/dev/full','wb') as f:p=subprocess.run([exe],input=data,stdout=f,stderr=subprocess.PIPE,timeout=5)
rows.append({'case':'output-ENOSPC','pass':p.returncode==2})
data=(d/'delayed-max.input').read_bytes();expected=(d/'delayed-max.expected').read_bytes();r,wr=os.pipe();size=fcntl.fcntl(wr,fcntl.F_SETPIPE_SZ,4096);os.set_blocking(wr,False)
p=subprocess.Popen([exe],stdin=subprocess.PIPE,stdout=wr,stderr=subprocess.PIPE);p.communicate(data,timeout=5);os.close(wr);got=os.read(r,65536);os.close(r)
rows.append({'case':'short-write-then-EAGAIN','pass':p.returncode==2 and 0<len(got)<len(expected) and got==expected[:len(got)],'written':len(got),'total':len(expected),'pipe_size':size})
print(json.dumps({'cases':len(rows),'results':rows},indent=2));assert all(x['pass'] for x in rows)
