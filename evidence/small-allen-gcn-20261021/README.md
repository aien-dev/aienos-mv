# Small Allen on gcn_interp: evidence (L389, 2026-10-21)

Base student weights (Qwen/Qwen3.5-0.8B @2fc06364, public) exported with `allen_train export-gpu`
(sha256 in `base.agp1.sha256`). Neutral prompt `prompt.txt` (no private data), ids in `ids.txt`.

- `host-reference-base.json`: `qwen38_check generate` (aien-inference-abi `Qwen38Reference`), 32 tokens.
- `interp-vs-mirror-base.json`: `allen_test base.agp1 ids.txt 32 12 both`: PASS, 33,756 launches byte-identical
  between gcn_interp and the host mirror, generated ids equal to the reference (32/32), per-kernel launch,
  instruction and byte counts. Interpreter 95.2 s, mirror 34.5 s on 12 Spark CPU threads (GB10 not used).

Rerun: see `native/kernel/gpu/kernels/allen/README.md`.
