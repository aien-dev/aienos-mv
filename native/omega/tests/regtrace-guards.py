import pathlib,subprocess,sys,os,json,tempfile
w=pathlib.Path(sys.argv[1]).resolve();cc=pathlib.Path(sys.argv[2]).resolve();exe=w/'build/omega-regtrace/regtrace';d=pathlib.Path(tempfile.mkdtemp(prefix='guard-',dir=exe.parent));builder=w/'native/omega/tests/regtrace.py';rows=[]
p=subprocess.run(['sh','-c','exec "$1" 0<&-','sh',str(exe)],capture_output=True);assert p.returncode==2;rows.append({'case':'closed-input','pass':True})
r,wr=os.pipe();os.set_blocking(r,False)
try:
 os.write(wr,b'\0');p=subprocess.run([str(exe)],stdin=r,capture_output=True,timeout=5);assert p.returncode==2;rows.append({'case':'read-after-progress-error','pass':True})
finally:os.close(r);os.close(wr)
victim=d/'victim';victim.write_bytes(b'KEEP');link=d/'symlink';link.symlink_to(victim);hard=d/'hardlink';os.link(victim,hard)
for name,target in [('symlink',link),('hardlink',hard),('source-overlap',w/'native/omega/regtrace.om')]:
 before=target.read_bytes();p=subprocess.run(['python3',str(builder),str(w),str(cc),str(target),'--build-only'],capture_output=True)
 assert p.returncode!=0 and target.read_bytes()==before and victim.read_bytes()==b'KEEP';rows.append({'case':name,'pass':True})
stale=d/'stale';stale.write_bytes(b'STALE');p=subprocess.run(['python3',str(builder),str(w),str(d/'missing-compiler'),str(stale),'--build-only'],capture_output=True);assert p.returncode!=0 and not stale.exists();rows.append({'case':'no-stale-output-on-missing-compiler','pass':True})
fifo=d/'fifo';os.mkfifo(fifo)
p=subprocess.run(['python3',str(builder),str(w),str(cc),str(fifo),'--verify-only'],capture_output=True,timeout=5);assert p.returncode!=0 and fifo.exists();rows.append({'case':'verify-fifo-refused','pass':True})
before=exe.read_bytes();p=subprocess.run(['python3',str(builder),str(w),str(d/'missing-compiler'),str(exe),'--verify-only'],capture_output=True,timeout=5);assert p.returncode!=0 and exe.read_bytes()==before;rows.append({'case':'verify-missing-compiler-preserves-checker','pass':True})
print(json.dumps({'cases':len(rows),'results':rows},indent=2))
