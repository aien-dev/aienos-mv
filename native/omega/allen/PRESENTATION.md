# L756 application presentation binding

Next attachment within L694 checkpoint5: use the qualified application, scene
normalizer and code-row layout instead of another parser or scripted UI.
No change to L748 provider source while its independent review is running.

`dask(now)` and `drecv(now)` call appask/appreceive respectively, preserve their
boolean admission result, then project only accepted event1 into presentation.
`dpresent()` consumes current Answer/R/A; it is called once per accepted event,
never to replay stale A2 after a failed operation. It returns true when a scene
or completed-code snapshot was updated. A rejected/hidden/empty answer leaves
the last accepted scene visible; it does not clear the display. Ordinary words
use the existing bounded plain fallback. Existing SAY folding/159-byte limit
remains; this does not establish multiline 2048-byte model scene transport.

Scene snapshots: Ds[8], Dt[64], Dtext[2304], Dsay[1]. Dm[0] scene valid,
Dm[1] completed code valid, Dm[2] last successfully projected conversation ID.
A validated completed-code event copies Code into Dcode[32768] and the existing
80-column/4096-row layout into Dstart/Dlen/Dline[4096], Dview[rows,top,bytes].
An in-progress/new/refused reply cannot corrupt the previous completed snapshot.
Code bytes are retained independently because codebeg/codetake reuse Code.

Input In/Inlen and offered Outb/Outlen/Err must survive presentation exactly.
Decoder scratch Sc/Things/Stext and view scratch may change; renderer consumers
must read the retained D* state. Pending transport state is never acknowledged.
`dreset()` calls appreset and zeros all retained presentation buffers/metadata;
conversation/wire counters retain the existing reset contract. No fabricated
answers, fonts, pixels, display device, disk or live socket implementation.

Qualification: compilable native stub RED, direct state checks and actual
ASK->courier LINE->drecv integration including pending offers, code completion,
invalid/hidden payloads, new-code mutation isolation and reset. Compile via the
pinned191b776e seed; add only module/test paths to the existing host build gate.
All behavioral decisions stay Omega. Independent review follows source freeze.
