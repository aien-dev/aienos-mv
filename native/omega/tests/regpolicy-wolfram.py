import pathlib,sys,json
p=pathlib.Path(sys.argv[1]);r=json.loads((p/'policy-green.json').read_text());w=json.loads((p/'policy-wolfram.json').read_text())['result']
assert r['failures']==0 and r['allowlist_equality']
assert r['read_allowed']==sorted(w['read']) and r['write_allowed']==sorted(w['write'])
assert w['magic']==int.from_bytes(b'RGP1','little') and w['inputMax']==4*(4+2*256) and w['outputMax']==4*(2+256)
print(json.dumps({'status':'PASS','read_registers':len(w['read']),'write_registers':len(w['write']),'magic':w['magic'],'inputMax':w['inputMax'],'outputMax':w['outputMax']}))
