# Physical Haswell evidence collector

`haswell-evidence.sh` performs the Stage 3C Check 15 read-only checks:

- target PCI address `0000:00:02.0`
- vendor/device `8086:0412`
- driver `i915`
- render node `/dev/dri/renderD128`

It does not unbind, reset, map, or write the GPU.

```sh
./tests/physical/haswell-evidence.sh [--force] [output-directory]
```

The collector refuses to overwrite a non-empty evidence directory unless
`--force` is passed.

or:

```sh
make physical-evidence PHYSICAL_EVIDENCE_DIR=path/to/output
```

Exit codes and result meanings are documented in
`docs/design/STAGE3C_PHYSICAL_EVIDENCE_RUNBOOK.md`.

A `TARGET_GPU_NOT_PRESENT` result is not physical evidence and cannot satisfy
Check 15. The 2026-11-21 bare-metal host run passed and is recorded in
`evidence-2026-11-21-host/`. That PASS satisfies Check 15 only; Stage 3C
remains open for the remaining checks.
