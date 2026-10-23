import pathlib,subprocess,sys,hashlib,json,re
w=pathlib.Path(sys.argv[1]).resolve();pin='83789354d963f584371ebbb2791f3d806f6cb281'
s=subprocess.check_output(['git','-C',str(w),'show',pin+':native/omega/bootseq.om'])
a=s.index(b'fn emem ');b=s.index(b'fn field ');core=(w/'native/omega/bootcore.om').read_bytes();adapter=(w/'native/omega/bootseq.om').read_bytes()
assert core==s[a:b] and adapter==s[:a]+s[b:]
functions=re.findall(rb'^fn (\w+)',core,re.M);assert len(functions)==6
print(json.dumps({'parent':pin,'byte_exact_core':True,'byte_exact_adapter_remainder':True,'functions':[x.decode() for x in functions],'core_sha256':hashlib.sha256(core).hexdigest()},indent=2))
