#!/usr/bin/env python3
"""External serial host qualifier. No signals/build failures count as detection."""
import hashlib,json,pathlib,subprocess,sys
root=pathlib.Path(__file__).resolve().parents[1];out=pathlib.Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=True)
p=root/'native/store/store_sealed.c';orig=p.read_text()
checks=[('plaintext-unbound','    sha256_hash(pt, len, ph);','    (void)pt; memset(ph, 0, sizeof ph);','independent ENVID oracle case 0'),('metadata-kind-unbound','    put16(m + 28, kind);','    (void)kind; put16(m + 28, 0);','independent ENVID oracle case 0'),('old-predictable-v1','    memcpy(m + 49, ph, 32);','    memcpy(m + 49, ph, 32);','independent ENVID oracle case 0'),('v1-refusal-removed','    if (get16(b + 8) == 1) return SS_E_FORMAT_VERSION; /* GUARD:tx-version-1 */','    /* mutant: no typed V1 refusal */','version-1 record refused'),('anchor-rollback-bypass','    case M5_ERR_ROLLBACK: return SS_E_ROLLBACK;','    case M5_ERR_ROLLBACK: break;','store gen 3 under anchor 4'),('anchor-inconsistent-bypass','    case M5_ERR_INCONSISTENT: return SS_E_INCONSISTENT;','    case M5_ERR_INCONSISTENT: break;','replayed anchor two behind')]
rows=[]
def run(args,name):
 try:r=subprocess.run(args,cwd=root,capture_output=True,text=True,timeout=180)
 except subprocess.TimeoutExpired:return None
 (out/(name+'.log')).write_text(r.stdout+r.stderr);return r
try:
 for name,old,new,msg in checks:
  if old not in orig:raise RuntimeError('unmatched '+name)
  text=orig.replace(old,new,1)
  if name=='old-predictable-v1':
   start=text.index('    uint8_t k[32], ph[32], m[',text.index('static int envid_derive'))
   end=text.index('\nint ss_prepare',start)
   text=text[:start]+'''    uint8_t m[28];
    sha256_ctx c;
    (void)kind; (void)version; (void)pt; (void)len;
    memcpy(m, s->uuid, 16); put64(m + 16, gen); put32(m + 24, index);
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"AIENOS-SEALED-ENVID-V1", sizeof("AIENOS-SEALED-ENVID-V1"));
    sha256_update(&c, m, sizeof m); sha256_final(&c, out); return 0;
}
''' +text[end:]
  p.write_text(text);build=run(['make','-C','native/store','OUT=out-crypto','out-crypto/store_sealed_test'],name+'-build');status='INVALID';rc=None;digest=None
  if build and build.returncode==0:
   binary=root/'native/store/out-crypto/store_sealed_test';digest=hashlib.sha256(binary.read_bytes()).hexdigest();r=run([str(binary),'native/store/out-crypto'],name+'-run')
   if r:
    rc=r.returncode;s=r.stdout+r.stderr
    if rc==1 and msg in s and 'STORE_SEALED: FAIL' in s and 'FAIL' in s:status='PASS'
    elif rc==0:status='FAIL'
  rows.append({'mutant':name,'status':status,'exit':rc,'assertion':msg,'binary_sha256':digest});print(json.dumps(rows[-1]),flush=True);p.write_text(orig)
finally:p.write_text(orig)
(out/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
sys.exit(0 if len(rows)==len(checks) and all(r['status']=='PASS' for r in rows) else 1)
