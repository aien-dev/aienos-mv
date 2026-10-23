# Native provider stream v1 (L748)

Builder choice: an explicitly host-development, bounded binary stream entry for
already-built Omega application functions. It is not a network, clock, storage
or device provider. All executable implementation and fixtures are Omega; host
shell only compiles and compares. The legacy development stdio main stays intact.

Input header:40 bytes, magic ASCII ALNPRV01, then four little-endian u64 fields:
operation, monotonic milliseconds, argument, body length. Body follows exactly.
No separators/padding after body. Header/body fragmentation is permitted.

| op | operation | argument | body |
|---|---|---|---|
|1|BOOT|nonzero unique run ID|empty|
|2|ASK|0|1..159 prompt bytes|
|3|POLL|link0/1|empty|
|4|SENT|actual whole-datagram success0/1|empty|
|5|RECV|0|65..1200 datagram bytes|
|6|CANCEL|0|empty; timestamp must0|
|7|RESET|0|empty; timestamp must0|
|8|COPY|slot0/1|4096 record bytes; timestamp0|
|9|NOTE|index0..511|4096 record bytes; timestamp0|
|10|LOAD|0|empty; timestamp0|

LOAD is optional before BOOT. It explicitly resets restore staging and begins a
new two-copy read. COPY accepts each slot once, valid or damaged, choosing the
newest valid memory copy with existing codec; both invalid means empty memory.
After both copies, ctxbegin runs. NOTE visits each declared slot once; damaged
records are visited as gone. BOOT refuses until restore is complete. LOAD may
restart restoration only before BOOT. COPY/NOTE/LOAD refuse after BOOT. Raw record
bytes come from a future provider; no physical persistence claim.

Output header uses same magic and four u64 fields: echoed op, status, application
event, body length. Status0 refused/no successful dispatch,1 accepted,2 accepted
record visit but damaged,3 accepted POLL with outgoing datagram. POLL without an
offer is status1, not refusal. On accepted non-offer event1, body is exact visible
Answer bytes. When that event also completes a valid code request, one additional
record op11/status1/event1 carries the exact Code bytes. No fabricated text.
## Retained presentation attachment (L763)

The actual provider dispatch uses dask for ASK, drecv for RECV and dreset for
RESET. Accepted event1 updates the retained D* scene/code state; malformed or
semantically refused operations never project stale event state. RESET clears
all retained presentation, while CANCEL retains it. BOOT still initializes once.
The source concatenation places presentation.om before provider-core.om because
the seed requires definitions before calls. There is no new operation or output
record: the external wire contract and exact answer/code bytes remain unchanged.
The renderer must consume retained D* buffers, not mutable assembly scratch.
This connects provider dispatch to display state, not yet to pixels or hardware.

Qualification: expand native prdo tests first (136 assertions;24 fail against
unchanged provider), bind the three existing calls, then replay the complete
host gate and three compiled call-bypass mutants. File/fragmented-pipe output
fixtures remain unchanged. Independent exact-source review follows publication.

The stream reply writer uses a separate buffer: application Outb/Outlen/Err remain
intact until SENT reports the real transport outcome. Writing an offer to stdout
never counts as sending its datagram.

Bad magic/op/unused fields/size => immediate exit65 before body or later command
execution. A well-framed semantic refusal produces status0 and permits subsequent
records. EOF between records is exit0; partial header/body is exit65. Read/write
errors are exit74 (inherited Linux signal disposition remains a separate limit).
No synthetic timestamps, link readiness, datagram send outcomes or storage writes.

Implementation/qualification sequence: native compilable stub + behavioral RED;
provider core with real application calls; native stream entry and real pipe/file
replay; valid workflow golden, partial/oversized/unknown frame refusal; combined
native gate and compiled mutations; immutable independent review. No target or
live-model acceptance from these host fixtures.
