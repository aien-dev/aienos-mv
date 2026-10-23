The canonical boundary.sha256 matches the final standalone probe,
receipts/font-boundary.om, font-boundary3.elf and308-row boundary.log. Parent
reconstructed that exact source closure from61ef587 Git objects, rebuilt a
byte-identical executable and reproduced all308 passing rows. focused.sha256
and failure-and-receipt-hashes.sha256 also match their original artifacts.

The separately named boundary-final.sha256 is a stale developmental receipt:
font-boundary-final.om was subsequently overwritten by a failed oversized-scratch
probe variant, so that one source hash no longer matches. This variant and its
original manifest are retained, not accepted as current qualification. The
canonical boundary.sha256 and parent replay above are the accepted evidence.
No product correction or gate relaxation was required. Original report retained.
