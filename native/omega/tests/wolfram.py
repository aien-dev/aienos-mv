import sys,pathlib,json,struct
p=pathlib.Path(sys.argv[1]);r=json.loads((p/'green.json').read_text());assert r['failures']==0
cases={x['case']:x for x in r['results']};w=json.loads((p/'wolfram.json').read_text())['result']
def state(name):
 b=bytes.fromhex(cases[name]['actual']);assert len(b)==w['outputBytes'];return struct.unpack('<21I',b)
assert state('modify-mask')[7]==w['modify'];assert state('delay-wrap')[3]==w['delayWrap']
for item in w['stuckReads']:assert state('stuck-'+str(item['budget']))[:2]==(1,item['reads'])
assert cases['max-words']['exit']==0 and cases['capacity']['exit']==2 and not cases['capacity']['actual']
print(json.dumps({'status':'PASS','wolfram_semantic_cases':2+len(w['stuckReads']),'boundary_cases':2}))
