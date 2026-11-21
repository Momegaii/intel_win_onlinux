# Stage 3C — D2 Ownership-Aware Probe-Failure Remediation Report

**Repository:** `Momegaii/intel_win_onlinux`
**Baseline:** `master` @ `730cba04979e1185bbf01b6a464c16cbbae930c6`
**Date:** 2026-11-21 UTC
**Status:** **D2 REMEDIATED — READY FOR INDEPENDENT REVIEW**
**Stage 3C closure:** **NOT CLAIMED** — Check 15 physical Haswell evidence remains unavailable.

This pass changes the QEMU EDU provider-owner lifetime, its QEMU regression
harness, and evidence. It does not activate a physical GPU, unbind i915, access
MMIO, change PCI ownership, or implement Stage 3D. QEMU EDU remains
virtual-device evidence, not Haswell hardware evidence.

---

## 1. Defect and governing invariant

D2 was classified as `UNSAFE_LIFETIME`.

Before this pass, `idw_edu_probe()` allocated `struct idw_edu_test` with
`kzalloc()`. The struct embeds the DMA provider and page-table adapter. When
provider or adapter finalization returned `-EBUSY`, the old `fail_provider`
path retained the allocation only as an unreachable local pointer. It then tore
down PCI resources even though the provider could still own pages, resources,
VMA references, or a device reference. A later probe failure allocated a new
owner; nothing bounded or reached the retained one. The old `remove()` path had
the same problem: it set `provider_ready = false` after a refused finalization
and returned without a reachable owner.

The governing invariant is now:

- An owner may be freed only after its adapter and provider have released all
  dependencies.
- PCI resources must not be released while an active DMA provider still owns
  dependencies.
- A refused finalization leaves the owner in a reachable, synchronized,
  bounded registry.
- Cleanup is retryable and ordered: misc/open references, adapter, provider,
  PCI resources, registry reference, owner reference.
- No new mappings, bindings, or allocations are accepted after cleanup starts.

## 2. Owner lifecycle

The implementation uses `enum idw_edu_owner_state`:

| State | Meaning | Transitions |
|---|---|---|
| `INITIALIZING` | Probe owns a new, unpublished owner. | `ACTIVE` on success; `CLEANUP_PENDING`/`QUARANTINED` on failure. |
| `ACTIVE` | Probe succeeded and the owner is published through `pci_set_drvdata()` and `idw_edu_active`. | `CLEANUP_PENDING` on remove. |
| `CLEANUP_PENDING` | Cleanup is actively running; new operations are stopped. | `QUARANTINED` if adapter/provider finalization refuses; `RELEASED` after all dependencies drain. |
| `QUARANTINED` | The owner is retained in the registry because a dependency remains. | `RECOVERING` when retry begins. |
| `RECOVERING` | A retry is about to re-check dependencies. | `CLEANUP_PENDING`; then `QUARANTINED` or `RELEASED`. |
| `RELEASED` | Dependencies are gone and PCI resources are released. | Terminal; registry reference and owner reference are dropped. |

`idw_edu_owner_cleanup()` is the single cleanup implementation used by probe
failure, remove, and the retry worker. It holds `lifecycle_lock` while stopping
new work and checking dependencies.

## 3. Registry and references

`struct idw_edu_test` now contains:

- `struct kref refs` for owner lifetime.
- `struct list_head registry_link`, `u64 owner_id`, `registered`, and
  `module_ref`.
- `lifecycle_lock` to serialize cleanup and state changes.
- PCI ownership flags `pci_enabled`, `pci_regions`, `pci_master`, and
  `bar_mapped`.
- Ordered-cleanup progress flags `adapter_cleanup_done`,
  `provider_cleanup_done`, and `pci_cleanup_done`.

The registry is `LIST_HEAD(idw_edu_owners)` protected by
`idw_edu_registry_lock`. It is bounded by `IDW_EDU_MAX_OWNERS` (8). A probe is
rejected with `-EBUSY` before allocation when the registry is full.
`idw_edu_registry_add()` takes a `kref` and a module reference, so a retained
owner remains reachable and prevents module unload. Registry removal drops the
registry reference only after the owner has reached `RELEASED`.

The module exposes read-only diagnostics:

- `owner_count` — retained owners currently in the registry.
- `owner_state` — state of the first retained owner.
- `test_result` — probe result.
- `probe_failure_test` — enables the actual probe-failure injection used by the
  QEMU regression harness.

A dedicated ordered workqueue retries quarantined owners once per second. Each
retry snapshots owners under the registry lock, takes a temporary `kref`, runs
ordered cleanup outside the registry lock, and drops the temporary reference.
The worker holds a temporary module reference while it runs.

## 4. Dependency-aware teardown order

`idw_edu_owner_cleanup()` now performs:

1. `misc_deregister()` and clears `idw_edu_active`; waits for an open file to
   close before releasing provider state.
2. `idw_dma_pt_adapter_fini()`. A refusal keeps the owner quarantined.
3. `idw_dma_provider_fini()` through `idw_edu_provider_fini_checked()`. A
   refusal keeps the owner quarantined and preserves PCI state.
4. `idw_edu_pci_cleanup()`: clear bus master, unmap BAR, release regions, and
   disable the PCI device.
5. `idw_edu_registry_remove()` and the owner reference drop.

The PCI cleanup is deliberately unreachable until adapter and provider
finalization succeed. In the injected probe-failure run, the retained owner
logs `pci_enabled=1 regions=1 master=1 bar=1` while the provider is blocked,
then logs PCI cleanup only after the injected page is released.

The old `remove()` path was also corrected: it no longer clears
`provider_ready` after a refused finalization, and it registers a quarantined
owner instead of dropping the last reachable pointer. A successful remove uses
the same PCI cleanup flags and ordered cleanup.

## 5. Actual probe-failure regression

The QEMU harness has two modes:

- Normal: `make qemu-dma-test` runs the full EDU probe, page-table DMA, native
  DMA fault/retry, and remove path.
- Probe failure: `tests/qemu-dma/run.sh --probe-failure` passes
  `idw_probe_failure_test=1` on the kernel command line. The guest init loads
  the test module with `probe_failure_test=1`.

In probe-failure mode, `idw_edu_probe()` allocates a coherent provider page and
then forces the real `fail_provider` path. The provider finalization returns
`-EBUSY`. The owner is retained in the registry as `QUARANTINED`, PCI resources
remain enabled, and the probe returns failure. The retry worker releases the
injected test page, moves the owner through `RECOVERING`, performs PCI cleanup,
and releases the owner. The guest init waits for `owner_count == 0`, unloads
the module, and repeats the entire cycle a second time. This exercises repeated
probe failures, reachability, bounded ownership, deferred PCI teardown, and
recovery.

The standard normal-mode test also runs a heap owner lifecycle regression that
holds an adapter page and a provider resource, observes staged `-EBUSY`
refusals, releases each dependency, and verifies final `RELEASED` with
`owner_count == 0`.

## 6. Verification

Evidence is in
`tests/qemu-dma/d2-ownership-remediation-2026-11-21/`.

| Command | Exit | Result |
|---|---:|---|
| `make kernel KDIR=/lib/modules/6.12.117+deb13-amd64/build CC=cc` | 0 | PASS |
| Software test group (`make contract-test ... cli-test`) | 0 | PASS for executed tests; native DMA tests SKIP without `/dev/idw-edu-dma` |
| `make ppgtt-model-test` | 0 | PASS |
| `make native-dma-fault-test` | 0 | SKIP outside QEMU |
| `make stage3a-test` | 0 | PASS |
| `make stage3a-sanitize-test` | 0 | PASS |
| `make qemu-dma-test` | 0 | PASS; QEMU status 33 |
| `tests/qemu-dma/run.sh --probe-failure` | 0 | PASS; QEMU status 33; two rounds |
| `make test` | **2** | Stops only at `tests/smoke.sh`: no Intel render node in this container |

Key QEMU probe-failure evidence:

```text
IDW_QEMU_DMA: owner 0 provider cleanup blocked: -16 pci_enabled=1 regions=1 master=1 bar=1
IDW_QEMU_DMA: probe failure retained owner 1 state=QUARANTINED count=1
IDW_QEMU_DMA: owner 1 released probe-failure test page
IDW_QEMU_DMA: owner 1 recovering from quarantine
IDW_QEMU_DMA: owner 1 PCI cleanup begin enabled=1 regions=1 master=1 bar=1
IDW_QEMU_DMA: owner 1 PCI cleanup complete enabled=0 regions=0 master=0 bar=0
IDW_QEMU_DMA: owner 1 released state=RELEASED
IDW_QEMU_DMA: probe failure round 1 owner recovered
IDW_QEMU_DMA: probe failure round 2 owner recovered
IDW_QEMU_DMA: PASS: probe failure cleanup, quarantine, retry, repeated owner release completed
```

## 7. Limitations

- QEMU EDU is virtual-device evidence. It is not Haswell hardware evidence.
- Check 15 physical Haswell/i915 evidence is unavailable in this container
  (`TARGET_GPU_NOT_PRESENT`). Stage 3C closure is not claimed.
- The current driver publishes one active EDU owner at a time. The registry
  supports multiple retained owners, but the QEMU harness tests one active
  device and repeated probe failures, not concurrent EDU devices.
- `probe_failure_test` and the injected page are test-only controls. They are
  not part of a production ownership path.
- `make test` still stops at the pre-existing smoke check because no Intel
  render node exists in the container.
- No push, merge, or pull request is part of this pass.

## 8. Files changed

| File | Change |
|---|---|
| `kernel/idw_edu_dma_test_main.c` | Lifecycle states, kref registry, bounded retries, ordered cleanup, PCI flags, diagnostics, actual probe-failure injection. |
| `tests/qemu-dma/guest_init.c` | Kernel command-line probe-failure mode, owner-count wait, two failure/recovery rounds. |
| `tests/qemu-dma/run.sh` | `--probe-failure` mode and evidence assertions. |
| `tests/qemu-dma/d2-ownership-remediation-2026-11-21/` | Logs, exit codes, and README. |
