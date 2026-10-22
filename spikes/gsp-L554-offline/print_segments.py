#!/usr/bin/env python3
"""Print copied GSP code using bounded PT_LOAD records, NOT stale section tables.
External inspection tooling only. Never program a GPU. Vendor bytes stay local.
"""
import argparse,gzip,hashlib,io,json,struct,subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
PIN='32cb7cf3e77f97e92f6e92135939ccb3ba382748144569c0dfccf3338d43d963'
OBJ='/usr/lib/llvm-18/bin/llvm-objdump'
EH='<16sHHIQQQIHHHHHH';PH='<IIQQQQQQ';SH='<IIQQQQIIQQ'

def sha(b):return hashlib.sha256(b).hexdigest()

def candidate(data,off):
    assert off+64<=len(data),'short ELF header'
    h=struct.unpack_from(EH,data,off)
    assert h[0][:7]==b'\x7fELF\x02\x01\x01','not ELF64 little-endian v1'
    assert h[1:4]==(2,243,1),'not RISC-V executable v1'
    assert h[8]==64 and h[9]==56 and 1<=h[10]<=32,'unsupported header dimensions'
    assert h[5]+56*h[10]<=len(data)-off,'program table out of image'
    segs=[];end=max(64,h[5]+56*h[10]);entryok=False
    for i in range(h[10]):
        s=struct.unpack_from(PH,data,off+h[5]+i*56)
        q=dict(type=s[0],flags=s[1],offset=s[2],vaddr=s[3],paddr=s[4],filesz=s[5],memsz=s[6],align=s[7])
        if q['type']==1:
            assert s[5]<=s[6] and s[2]+s[5]<=len(data)-off,'invalid file-backed extent'
            assert s[3]+s[6]<=1<<64,'virtual address overflow'
            assert s[7] in (0,1) or (s[7]&(s[7]-1)==0 and s[2]%s[7]==s[3]%s[7]),'invalid segment alignment'
            end=max(end,s[2]+s[5])
            entryok |= bool(s[1]&1 and s[3]<=h[4]<s[3]+s[5])
        segs.append(q)
    assert entryok,'entry is not file-backed executable memory'
    return dict(offset=off,extent=end,entry=h[4],flags=h[7],section_table_offset=h[6],section_count=h[12],segments=segs)

def wrapper(code,va,flags):
    """Analysis-only .text wrapper; every instruction byte remains unchanged."""
    names=b'\0.text\0.shstrtab\0';ns=64+len(code);so=(ns+len(names)+7)&~7
    ident=b'\x7fELF\x02\x01\x01'+bytes(9)
    eh=struct.pack(EH,ident,1,243,1,0,0,so,flags,64,0,0,64,3,2)
    sh=bytes(64)+struct.pack(SH,1,1,6,va,64,len(code),0,0,2,0)+struct.pack(SH,7,3,0,0,ns,len(names),0,0,1,0)
    return eh+code+names+bytes(so-ns-len(names))+sh

def run(args):
    p=subprocess.run(args,capture_output=True,text=True,timeout=40)
    assert p.returncode==0,(args,p.returncode,p.stderr)
    return p.stdout+p.stderr

def main():
    ap=argparse.ArgumentParser();ap.add_argument('snapshot',type=Path);a=ap.parse_args();out=a.snapshot
    raw=(out/'gsp_ga10x.bin').read_bytes();assert sha(raw)==PIN
    outer=ELFFile(io.BytesIO(raw));fs=outer.get_section_by_name('.fwimage');image=fs.data()
    derived=out/'fwimage.bin'
    if derived.exists():assert image==derived.read_bytes()
    else:derived.write_bytes(image);derived.chmod(0o444)
    prefix=run([OBJ,'-D','--section=.fwimage','--start-address=0','--stop-address=512','--mattr=+m,+a,+f,+d,+c',str(out/'gsp_ga10x.bin')])
    (out/'boot-prefix-disassembly.txt').write_text(prefix)
    outersections=[dict(name=s.name,offset=s['sh_offset'],size=s['sh_size']) for s in outer.iter_sections()]
    objs=[];rejects=[];pos=0
    while True:
        pos=image.find(b'\x7fELF',pos)
        if pos<0:break
        try:objs.append(candidate(image,pos))
        except (AssertionError,struct.error) as ex:rejects.append(dict(offset=pos,reason=str(ex)))
        pos+=4
    for q,nxt in zip(objs,objs[1:]):assert q['offset']+q['extent']<=nxt['offset'],'overlapping load extents'
    for q in objs:
        stem='load-%08x'%q['offset'];chunk=image[q['offset']:q['offset']+q['extent']]
        q['sha256']=sha(chunk);q['filename']=stem+'.payload';(out/q['filename']).write_bytes(chunk)
        q['section_table_within_payload']=q['section_table_offset']+q['section_count']*64<=len(chunk)
        listings=[]
        for i,s in enumerate(q['segments']):
            if s['type']!=1 or not(s['flags']&1):continue
            code=chunk[s['offset']:s['offset']+s['filesz']];fn=stem+f'-exec-{i}.elf';path=out/fn
            path.write_bytes(wrapper(code,s['vaddr'],q['flags']))
            # Crosscheck synthetic wrapper bytes and addresses with independent readelf.
            hdr=run(['readelf','-h','-SW',str(path)]);(out/(fn+'.headers.txt')).write_text(hdr)
            wrapped=ELFFile(io.BytesIO(path.read_bytes()));text=wrapped.get_section_by_name('.text')
            assert text.data()==code and text['sh_addr']==s['vaddr']
            listing=fn+'.asm.gz'
            with gzip.open(out/listing,'wb',compresslevel=1) as dst:
                p=subprocess.Popen([OBJ,'-d','--mattr=+m,+a,+f,+d,+c',str(path)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                while True:
                    block=p.stdout.read(1024*1024)
                    if not block:break
                    dst.write(block)
                err=p.stderr.read();assert p.wait(timeout=60)==0,err
            excerpt=run([OBJ,'-d','--mattr=+m,+a,+f,+d,+c','--start-address='+str(q['entry']),'--stop-address='+str(q['entry']+96),str(path)])
            listings.append(dict(segment=i,gzip=listing,bytes_sha256=sha(code),entry_listing=excerpt))
        q['listings']=listings
    result=dict(firmware_sha256=PIN,firmware_bytes=len(raw),fwimage_offset=fs['sh_offset'],fwimage_bytes=len(image),fwimage_sha256=sha(image),objects=objs,rejected_magic_hits=rejects,method='bounded ELF program headers; section tables outside load extents are not recovered',outer_sections=outersections)
    (out/'binary-map.json').write_text(json.dumps(result,indent=2)+'\n')
    lines=['# GB10 GSP: snapshot and binary print','',
      'Readable instruction listings from copied firmware. NOT the original source and NOT replacement firmware.',
      'No device access, firmware install, module unload, or reset by these tools.',
      '',f'Original bytes: {len(raw)}',f'Original SHA-256: `{PIN}`',
      f'.fwimage offset: `0x{fs["sh_offset"]:x}`; bytes: {len(image)}','',
      '## Embedded executable payloads','',
      'The payloads retain ELF headers and load segments but omit the original section tables.',
      'Following their stale section pointers crosses into the next payload. They must not be read as ordinary intact ELF files.',
      'Listings use analysis-only .text wrappers around unchanged executable-segment bytes. Executable segments can include data/padding; linear disassembly is not proof every line is reachable code.',
      '', '| .fwimage offset | Load bytes | Entry VA | Original section table retained |', '|---|---:|---|---|']
    for q in objs:lines.append(f'| `0x{q["offset"]:x}` | {q["extent"]} | `0x{q["entry"]:x}` | {q["section_table_within_payload"]} |')
    lines+=['',f'Validated loadable payloads: {len(objs)}; rejected incidental magic matches: {len(rejects)}.',
            'This is an ELF load-segment inventory, not a claim that every remaining byte is classified.','',
            '## Container sections','', '| Name | Offset | Bytes |','|---|---|---:|']
    for s in outersections:lines.append(f'| `{s["name"]}` | `0x{s["offset"]:x}` | {s["size"]} |')
    lines+=['','## Candidate bootstrap prefix','Entry at raw offset zero and emulator load base are hypotheses, not a verified reset state.',
            '```text',(out/'boot-prefix-disassembly.txt').read_text(),'```']
    for q in objs:
        lines+=['',f'## Payload at `0x{q["offset"]:x}`',f'SHA-256 `{q["sha256"]}`','',
                '| Program type | Flags | File offset | VA | File bytes | Memory bytes |','|---|---|---|---|---:|---:|']
        for s in q['segments']:lines.append(f'| `0x{s["type"]:x}` | {s["flags"]} | `0x{s["offset"]:x}` | `0x{s["vaddr"]:x}` | {s["filesz"]} | {s["memsz"]} |')
        for z in q['listings']:lines+=['',f'Full executable-segment print: `{z["gzip"]}`','```text',z['entry_listing'],'```']
    (out/'PRINT.md').write_text('\n'.join(lines)+'\n')
    print(json.dumps(dict(payloads=len(objs),rejected_magic=len(rejects),stale_section_tables=sum(not q['section_table_within_payload'] for q in objs),print=str(out/'PRINT.md'),disassembly_gzip_bytes=sum((out/z['gzip']).stat().st_size for q in objs for z in q['listings'])),indent=2))

if __name__=='__main__':main()
