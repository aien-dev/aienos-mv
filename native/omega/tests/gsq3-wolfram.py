import pathlib,json,struct,sys
p=pathlib.Path(sys.argv[1]);r=json.loads((p/'gsq3-green.json').read_text());w=json.loads((p/'gsq3-wolfram.json').read_text())['result'];assert r['failures']==0
rows={x['case']:x for x in r['results']}
def values(name):
 x=rows[name];assert x['pass'];return struct.unpack('<28I',bytes.fromhex(x['actual']))
x=values('write-modify-poll-save');assert list(x[:6])==w['successCounts'] and x[10]==x[20]==w['modified255']
x=values('modify-doorbell');assert x[5]==1 and x[11]==w['modified105']
x=values('timeout-does-not-consume-future-bell');assert x[0]==1 and x[1]==w['stuckBudget1Reads'] and x[5]==0
x=values('prior-bell-survives-timeout');assert x[0]==1 and x[1]==w['stuckBudget1Reads'] and x[5]==1
assert w['magic']==int.from_bytes(b'GSQ3','little') and w['inputMax']==4*(30+1024) and w['outputBytes']==4*28
print(json.dumps({'status':'PASS','native_cases':4,'framing_checks':3}))
