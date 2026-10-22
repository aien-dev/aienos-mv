import pathlib,subprocess,sys,hashlib,struct,json
w=pathlib.Path(sys.argv[1]).resolve();cc=pathlib.Path(sys.argv[2]).resolve();out=pathlib.Path(sys.argv[3]).absolute()
assert not any(x.is_symlink() for x in [out,*out.parents]),'unsafe output symlink'
out=out.resolve()
assert out not in [pathlib.Path(__file__).resolve(),cc,w/'native/omega/regcore.om',w/'native/omega/regtrace.om',w/'native/linux/aien-gpu/aien_gpu.c'],'output overlaps input'
if out.exists():
 assert out.is_file() and out.stat().st_nlink==1,'unsafe output file'
 out.unlink()
out.parent.mkdir(parents=True,exist_ok=True)
assert hashlib.sha256(cc.read_bytes()).hexdigest()=='01b1545b23e8ff40cb5691e20b5df910ea4acaefde3495f0d62d270465b98cd6'
assert hashlib.sha256((w/'native/linux/aien-gpu/aien_gpu.c').read_bytes()).hexdigest()=='10990a78ff93af70f8a7c2f36fb92fa05d8a53f787f70df77dfd73f28db6d5d6'
s=b'\n'.join((w/'native/omega'/x).read_bytes() for x in ['regcore.om','regtrace.om']);p=subprocess.run([str(cc)],input=s,capture_output=True);assert p.returncode==0,p.stderr
assert p.stdout[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<HH',p.stdout,16)==(2,183)
ph=struct.unpack_from('<Q',p.stdout,32)[0];ps,pn=struct.unpack_from('<HH',p.stdout,54);assert ps==56 and pn and ph+ps*pn<=len(p.stdout);assert not {2,3}.intersection(struct.unpack_from('<I',p.stdout,ph+i*ps)[0] for i in range(pn))
out.write_bytes(p.stdout);out.chmod(0o700)
if '--build-only' in sys.argv:sys.exit(0)
def line(op,addr,value=0,t='0'):return f'{op} {addr:06x} {value:08x} t={t}\n'.encode()
reads=[0,2560,8650816,8650820,8653504,8653824,8653828,1114176,1114180,1114240,1114356,1117184,1119112];writes=[8653504,8653508,8653824,8653828,1114240,1117184]
good=b''.join(line('W',x,0xffffffff) for x in writes)+b'\0'
cases=[('all-writes',good,0),('all-reads',b''.join(line('R',x) for x in reads)+good,0),('two-startups',good+good,0),('empty-traces',b'\0\0'+good,0),('missing-coverage',line('W',writes[0])+b'\0',1),('double-doorbell',line('W',8653824)+good,1),('read-writeonly',line('R',8653508)+good,1),('unknown-read',line('R',0xffffffff)+good,1),('unaligned-write',line('W',8653825)+good,1),('empty',b'',2),('unterminated',good[:-1],2),('refused-marker',line('W',writes[0])[:-1]+b' REFUSED\n'+good,2),('bad-hex',b'R 00000g 00000000 t=0\n'+good,2),('short-address',b'R 00000 00000000 t=0\n'+good,2),('long-address',b'R 000000000 00000000 t=0\n'+good,2),('short-value',b'R 000000 0000000 t=0\n'+good,2),('no-time',b'R 000000 00000000 t=\n'+good,2),('long-time',line('R',0,t='1'*21)+good,2),('twenty-digits',line('R',0,t='9'*20)+good,0),('crlf',line('R',0).replace(b'\n',b'\r\n')+good,2),('nul-mid-record',b'R \0'+good,2),('capacity',b'\0'*(65536-len(good))+good,0),('over-capacity',b'\0'*(65537-len(good))+good,2)]
rows=[]
for name,data,status in cases:
 p=subprocess.run([str(out)],input=data,capture_output=True,timeout=10);rows.append({'case':name,'pass':p.returncode==status and not p.stdout,'exit':p.returncode,'expected':status})
print(json.dumps({'cases':len(rows),'failures':sum(not x['pass'] for x in rows),'source_sha256':hashlib.sha256(s).hexdigest(),'binary_sha256':hashlib.sha256(out.read_bytes()).hexdigest(),'results':rows},indent=2));sys.exit(any(not x['pass'] for x in rows))
