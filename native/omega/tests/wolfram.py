import sys,pathlib,json,struct
p=pathlib.Path(sys.argv[1]);r=json.loads((p/'v2-green.json').read_text());assert r['failures']==0
cases={x['case']:x for x in r['results']};w=json.loads((p/'v2-wolfram.json').read_text())['result']
def state(name):
 b=bytes.fromhex(cases[name]['actual']);assert len(b)==w['outputBytes'];return struct.unpack('<21I',b)
assert state('modify-mask')[7]==w['modify'];assert state('delay-wrap')[3]==w['delayWrap']
for item in w['stuckReads']:assert state('stuck-'+str(item['budget']))[:2]==(1,item['reads'])
preflight=['partial-effects','preflight-unknown','preflight-save','preflight-disabled-core','preflight-truncated']
for name in preflight:assert list(state(name))==w['preflightState']
assert w['magic']==int.from_bytes(b'GSQ2','little') and w['inputMax']==4*(22+1024)
assert cases['max-words']['exit']==0 and cases['capacity']['exit']==2 and not cases['capacity']['actual']
print(json.dumps({'status':'PASS','wolfram_semantic_cases':2+len(w['stuckReads'])+len(preflight),'boundary_cases':2,'version_magic':w['magic']}))
