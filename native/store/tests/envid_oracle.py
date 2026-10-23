#!/usr/bin/env python3
"""Independent oracle for the fixed synthetic #445 fixtures, stdout JSON."""
import hashlib,hmac,json,struct
uuid=bytes.fromhex('5ea1ed00112233445566778899aabbcc');key=bytes([0x23])*32
k=hmac.digest(key,b'AIENOS-SEALED-ENVID-KEY-V2\0\x01','sha256');cases=[]
for i in range(9):
 u=bytes([uuid[0]^1])+uuid[1:] if i==1 else uuid
 gen=4 if i==2 else 3;index=1 if i==3 else 0;kind=43 if i==4 else 42;ver=2 if i==5 else 1;kg=2 if i==6 else 1;cls=2 if i==7 else 1;pt=b'synthetic!' if i==8 else b'synthetic'
 m=u+struct.pack('<QIHHQBQ',gen,index,kind,ver,kg,cls,len(pt))+hashlib.sha256(pt).digest()
 digest=hmac.digest(k,b'AIENOS-SEALED-ENVID-V2\0'+m,'sha256')
 cases.append({'case':i,'uuid':u.hex(),'generation':gen,'index':index,'kind':kind,'version':ver,'key_generation':kg,'class':cls,'plaintext_hex':pt.hex(),'hmac_sha256':digest.hex()})
print(json.dumps({'schema':'AIEN-ENVID-ORACLE-1','domain_key_hex':key.hex(),'hkdf_expand_hex':k.hex(),'cases':cases},indent=2))
