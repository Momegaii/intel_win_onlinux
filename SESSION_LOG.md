# Development session log

Date: 2026-11-20

## Requested development sequence

The project roadmap now states the requested order:

1. GPU memory management: real GPU virtual addresses, page-table mappings, residency, and eviction.
2. Haswell Gen7.5 PPGTT, without Gen8+ assumptions.
3. Engine-specific hardware submission, command execution, and completion fences.
4. Scheduler: contexts, queues, preemption limits, timeouts, and recovery.
5. UMD: resources, shader compilation, and Gen7.5 command generation.
6. Independent display/KMS integration.
7. Leave i915 ownership only after the independent backend is ready.

The sequence was recorded in `README.md`, `docs/IMPLEMENTATION_STATUS.md`,
`docs/design/STACK_CONTRACTS.md`, and `PROJECT_STATE.txt`.

## i915-assisted GPU-memory validation

Added the opt-in `idwctl gpu-memory-test` command, `make gpu-memory-test`
target, and `tests/gpu-memory-smoke.sh`. The test source:

- Requires Haswell device `8086:0412` with i915 as the current owner.
- Creates GEM resource and batch objects and requests fixed context GPU virtual
  addresses using `EXEC_OBJECT_PINNED`.
- Checks the returned offsets, submits a harmless batch, and waits for
  completion.
- Destroys the context before exercising GEM backing advice.
- Checks backing-retention results, recreates the resource if backing has been
  purged, verifies its contents, and attempts a fresh-context rebind at the same
  GPU addresses.
- Closes GEM handles and the DRM file descriptor on exit paths.

This demonstrates only source-implemented i915-assisted validation. i915 still
performs the page-table mapping, residency, and eviction work. `DONTNEED` allows
backing to be discarded; it does not force eviction. The test does not prove
IDW-owned PPGTT, IDW-controlled mapping/residency/eviction, independent address
translation, or independent GPU execution.

## Focused purge-recovery audit

Audited the test's advice, context, and cleanup paths against upstream Linux
i915 UAPI and implementation sources. The UAPI defines `retained` as whether
backing still exists; i915's implementation sets it from the object's current
purge state. `DONTNEED` marks backing discardable and does not guarantee
immediate purge.

The audit found a race: backing could be retained when `DONTNEED` returned,
then purged before the following `WILLNEED`. The test now treats
`retained == 0` from either ioctl as purged and follows the recovery path.
Advice failures stop the test. Context destruction is attempted before backing
advice and retried during cleanup if needed. Partial submission, wait, advice,
recreation, or rebind failures still flow through context/GEM cleanup; closing
the DRM fd remains the final kernel cleanup boundary.

## Validation and hardware status

Software checks passed:

- `make all contract-test abi-test`
- `bash -n tests/gpu-memory-smoke.sh`
- `./build/idwctl --help`

The build emitted existing `-Wpedantic` warnings from flexible/zero-length
members in the system libdrm i915 header; the build and tests completed.

**The physical `gpu-memory-test` was not run.** There is no hardware validation
result. Passing software checks is evidence about software correctness, not
proof of independent GPU control.

## PPGTT planning (historical checkpoint)

- i915-assisted memory validation: extended in source, not physically tested.
- Context and backing lifecycle testing: extended in source, not physically
  tested.
- IDW GPU VA manager: still required; not implemented.
- IDW-owned Haswell Gen7.5 PPGTT: still required; not implemented.
- Hardware page-table validation: still required; not run.
- i915 remains the physical GPU owner; no unbind, MMIO programming, or
  independent GPU-control claim was made.

Added `docs/design/HASWELL_GEN75_PPGTT_PLAN.md`, which sets independence as the
architecture target while retaining development safeguards. It defines the
user's 10 phases, CPU-model-to-hardware validation gates, VidMm/GPU-VA versus
PPGTT/backend ownership, rollback and poison-on-failed-invalidation rules, and
the isolated hardware ownership boundary.

The plan records an upstream Linux v6.12 i915 Gen6/7 reference baseline:
32-bit PTEs, 4 KiB pages, PDE shift 22, 1024 PTEs per table, and a 2 GiB
address-space iteration range. Exact Haswell PRM bit encoding, DMA address
width, cache policy, invalidation sequence, and engine activation remain
research gates before hardware implementation.

Added compile-checked `include/idw/contracts/haswell_ppgtt.h` contracts for DMA
page memory, mapped allocation segments, page-table synchronization, Gen7.5
capability reporting, VM invalidation, and per-engine activation/deactivation.
Extended backend capability reporting with PPGTT generation/address width.
Updated implementation status and complete-stack dependency docs. These
interfaces and the plan are not a CPU table model or runtime PPGTT.

Validation after these additions: `make all contract-test abi-test` passed.
The physical `gpu-memory-test` remains unrun. No CPU table model, DMA mapping,
PTE/PDE writer, MMIO/invalidation implementation, hardware test, or i915
ownership transition has started.

## Gen7.5 CPU model implementation update

The earlier status above is historical and is superseded by this
implementation update. Implemented the first substantive CPU-side GPU-VA/PPGTT
core:

- `src/gpuva_model.c` / `include/idw/gpuva_model.h`: independent CPU-only
  first-fit GPU-VA interval allocator with alignment, guard reservations,
  fixed reserve, overlap/range/overflow rejection, and free.
- `src/haswell_ppgtt_model.c` /
  `include/idw/haswell_ppgtt_model.h`: 512-entry legacy Gen7 directory model;
  4 KiB PTE pages with 1024 entries; upstream Gen6/7 40-bit address encoding
  `addr | ((addr >> 28) & 0xff0)` and valid bit 0; Haswell uncached PTE policy;
  checked map/unmap and software VA-to-DMA translation.
- Map builds missing page tables off-tree and rolls back on allocation failure,
  duplicate DMA table-page address, malformed range, duplicate mapping, or
  malformed DMA page. Unmap validates the full range before mutation, retains
  empty tables until the matching invalidation generation completes, and
  invalidation failure poisons the model so software submission checks reject
  use. Dirty ranges are coalesced; stale generation acknowledgements are
  rejected.
- `tests/ppgtt-model-test.c` exercises VA geometry/guards/overlap, independent
  golden PTE address vectors, translation across PDE boundaries, malformed
  maps, allocation and duplicate-DMA rollback, no-partial unmap, delayed table
  reclamation, invalidation generation/coalescing, and poison-on-failure.
- `make ppgtt-model-test` builds with strict C11 warnings and runs that suite.
  The normal `make test` target includes it.

The implementation maintains subsystem boundaries: the GPU-VA allocator does
not own VidMm backing, residency, or allocation references; the PPGTT model
does not allocate/manage backing or GPU VA ranges; neither does device DMA
mapping, page-table synchronization, hardware invalidation, engine activation,
or MMIO. The test page provider returns synthetic DMA addresses. Software
translation is not a GPU page walk or independent GPU control.

Gen6/7 entry definitions were source-checked against upstream Linux v6.12
`drivers/gpu/drm/i915/gt/intel_gtt.h` and `gen6_ppgtt.c`; Haswell's exact PRM
requirements, permission policy, cache alternatives, physical page-directory
layout/alignment, and hardware invalidation remain gates before hardware use.

Validation completed after implementation: `make all`, `make contract-test`,
`make abi-test`, `make ppgtt-model-test`, and AddressSanitizer/UndefinedBehavior
Sanitizer unit tests. Physical `gpu-memory-test` remains unrun. i915 remains
the GPU owner; no physical GPU takeover or hardware PPGTT programming occurred.

Final verification rerun after the output-pointer and GPU-VA overflow-test
changes passed: `make all`, `make contract-test`, `make abi-test`,
`make ppgtt-model-test`, and the ASan/UBSan PPGTT test binary. The model tests
explicitly use synthetic DMA addresses; physical GPU validation remains unrun.

## Project-wide source audit and validation

Reviewed active user-space, kernel, contract, UAPI, test, and project-status
files; `legacy-prototype/` remains historical reference code. Its shell script
was syntax-checked, but the prototype C/C++ code was not promoted into or
compiled as part of the active implementation.

Corrections made:

- Replaced permissive `stoul`/`stoull` parsing and narrowing casts with strict
  decimal parsing, range checks, and rejection of unexpected arguments. Added
  `tests/cli-arguments-smoke.sh` and wired it into `make test`; invalid inputs
  are confirmed to fail before opening hardware/device nodes.
- Added `include/uapi/idw_ioctl.h` as an `idwctl` build dependency so UAPI
  changes cannot leave a stale user binary.
- Reordered the NT-inspired Unicode shim's bounded scan condition so the cap is
  checked before dereferencing the next code unit.
- Changed compat ioctl dispatch to `compat_ptr_ioctl` for the kernel's
  architecture-correct userspace pointer conversion.
- Removed duplicate sysfs reads in the GUI's fallback metric selection, avoiding
  inconsistent "not exposed" results if a telemetry file changes between reads.
- Replaced exception-based hex parsing with checked `from_chars` parsing and
  corrected the README's stale/duplicated next-gates list to reflect the
  current GPU memory manager → Gen7.5 PPGTT → submission/fences → scheduler →
  UMD → display → compatibility/ownership order.

Validation on 2026-11-20:

- `make test` passed, including ABI/contracts, CPU-only GPU-VA/PPGTT tests,
  CLI regression tests, discovery, i915 GEM selftest, and bounded GEM lifecycle
  test. The tests that require `/dev/idw` skipped because the module was not
  loaded.
- `make kernel` compiled the kernel modules successfully; no module was loaded.
- All project shell scripts passed `bash -n`; the GUI and debugfs Python files
  passed Python AST parsing after the GUI telemetry-read correction.
- GCC `-fanalyzer` syntax checks for the active C/C++ user-space sources passed
  without findings. The build's remaining warnings are from flexible/zero-length
  array declarations in the external system libdrm header.

## Page-table memory backend boundary

Implemented the backend-neutral page-table-memory contract and its two backend
contracts:

- `page_table_memory.h` defines device-generation-scoped DMA pages/mappings,
  allocation and mapping lifetime cookies, memory type, capability bits, and
  alloc/map/unmap/synchronization callbacks.
- `src/page_table_memory.c` validates backend capability coherence, requires
  exclusive ownership for native DMA, rejects i915 claims to native DMA/PPGTT,
  checks the Haswell 40-bit/alignment/page-size constraints, and rejects
  device-scope mismatches.
- `i915_assisted.h` and `src/i915_assisted_backend.c` report only the existing
  i915 GEM/dma-buf/context-VM/exec-wait validation surface. They explicitly do
  not expose i915's DMA mappings, PPGTT tables, synchronization, invalidation,
  or activation.
- `native_dma.h` defines the future independent backend contract. Its validator
  checks exclusive ownership and complete page-allocation, resource-map,
  synchronization, invalidation, and activation hooks. It does not implement
  those operations or bind a PCI device.
- The CPU PPGTT model now tags table allocations and mapped PTE pages with
  provider/device generation and lifetime cookies, rejecting cross-scope
  providers or mappings before changing page tables.
- Added `make backend-contract-test`, compiled in `make test`, with coverage
  for i915 capability rejection, native ownership/capability requirements,
  missing callbacks, malformed pages, and wrong device/generation mapping
  rejection. The PPGTT model suite adds wrong-scope provider and mapping cases.

Validation passed: `make test`, GCC `-fanalyzer` on all changed C contract/model
sources, and ASan/UBSan runs for both contract and PPGTT tests. All new backend
tests use synthetic device scopes, DMA addresses, and callbacks; they do not
call Linux DMA APIs. No real DMA mapping, synchronization, hardware
invalidation, GPU PPGTT publication, GPU submission, or ownership transition
was performed. i915 remains the physical device owner.
- ASan/UBSan PPGTT model tests passed with leak detection enabled.
- The generic suite did create/write/read/close i915 GEM objects, but did not
  submit GPU commands. `gpu-memory-test`, `gpu-submit`, `gpu-resource-test`,
  and the kernel-dispatch load/unload smoke test were not run. Physical GPU
  memory/PPGTT validation remains unrun; i915 remains the hardware owner.

## i915-assisted GEM resource backend verification

Implemented `include/idw/i915_gem_resource_backend.hpp` and
`src/i915_gem_resource_backend.cpp` as a user-space resource manager over
public DRM/i915 GEM ioctls. It tracks GEM handles and rounded object sizes,
`WILLNEED`/`DONTNEED` backing-advice results, `GEM_BUSY` activity samples, and
context GPU offsets returned by successful i915 execbuffers. It does not
implement or claim a native DMA provider, GPU residency query, or IDW-owned
PPGTT.

Added simulated-ioctl unit tests in
`tests/i915-gem-resource-backend-test.cpp` and the real,
non-submission `idwctl gem-resource-test` / `tests/gem-resource-backend-smoke.sh`.
The real smoke test passed on the current i915 device: it created a GEM
resource, checked backing advice, observed that backing had been purged,
recovered it, sampled GEM_BUSY as idle, verified the resource data, and closed
the object. No GPU command was submitted. `GEM_BUSY` is in-flight activity,
not residency; `DONTNEED` permits discard under memory pressure and is not
forced eviction.

The distinct `gpu-memory-test` remains unrun. It exercises i915 context
GPU-address placement and submits a no-op batch; its physical execution is not
proven by the non-submission GEM smoke test. i915 continues to own DMA,
PPGTT, GPU mappings, placement/residency, and command execution.

Validation completed after the backend changes:

- `make test` passed, including contracts, PPGTT model, simulated resource
  backend tests, CLI validation, real GEM round-trip/resource/lifecycle smoke
  tests. Tests requiring `/dev/idw` skipped because the module was not loaded.
- `idwctl gem-resource-test` passed against the physical i915 render node.
- Backend unit tests passed under ASan/UBSan with leak detection.
- GCC `-fanalyzer` passed for the changed user-space C++ sources.
- Project shell-script and Python syntax checks passed.
- The build retains `-Wpedantic` warnings originating in system libdrm headers.

No native DMA operation, IDW-owned PPGTT mapping, GPU command submission by
this new smoke test, independent residency/eviction control, or ownership
transition was performed.

## Project-wide source and build audit

Reviewed active project sources, build/test targets, status documentation,
shell/Python entry points, and the archived `legacy-prototype` user-space
targets. The audit found stale UAPI include/dependency paths in the archived
client, shim, installer, and kernel prototype, pointing to a nonexistent
`../include/idw_uapi.h`. Updated them to use the adjacent `idw_uapi.h`, added
the missing Kbuild module entry, corrected the kernel path to use `CURDIR`, and
rewired the aggregate makefile to call local build files.

Final validation:

- `make test` passed. The i915 GEM smoke tests ran; the `/dev/idw` query,
  allocation, and dma-buf import checks skipped because the module is not
  loaded.
- `make kernel` compiled the active kernel modules through Kbuild; no module
  was loaded.
- Active and legacy user-space C/C++ syntax checks passed.
- All project shell scripts passed `bash -n`; the GUI and debugfs Python
  sources passed Python AST parsing.
- GCC `-fanalyzer` passed for active and legacy user-space C/C++ sources.
- ASan/UBSan with leak detection passed for the CPU PPGTT model, page-table
  contract tests, and i915 GEM backend tests.
- The archived aggregate build compiled the legacy kernel module, user-space
  client/shim, and installer (using host GCC for the installer), then cleaned
  all generated products successfully. The archived module was not loaded.

`cppcheck` and `shellcheck` were not installed; no package was added. The
project build still emits `-Wpedantic` diagnostics from flexible/zero-length
array declarations in the external system libdrm headers. These are warnings,
not project-source syntax errors. The archived kernel prototype is only
build-validated and is not an active hardware backend.

No command-submission hardware test was run: `gpu-memory-test`, `gpu-submit`,
and `gpu-resource-test` remain opt-in and unrun. No native DMA operation,
IDW-owned PPGTT, independent residency/eviction, or ownership transition was
validated; i915 remains the physical GPU owner.

## GPU execution milestone — software and hardware evidence

Implemented `include/idw/gpu_submission.hpp` and `src/gpu_submission.cpp` as a
replaceable submission contract with the current public-ioctl i915 backend in
`idwctl`. The Haswell-specific harmless command buffer is exactly two dwords:
`MI_BATCH_BUFFER_END` (`0x05000000`) followed by `MI_NOOP` padding (`0`). The
exact validator rejects other contents and lengths. The 8-byte command-buffer
length is required by the tested i915 path; the former 4-byte submission
returned `EINVAL`.

The submission state machine creates a context and GEM batch, uploads the
validated command, submits through render `GEM_EXECBUFFER2`, validates the
i915-returned GPU VA for nonzero 4 KiB alignment, and waits up to five seconds
using `GEM_WAIT`. It closes the GEM handle and destroys the context on success,
failed upload/submission, timeout, and invalid returned address. Cleanup errors
are surfaced separately from the operation error. It accesses no i915-private
DMA mapping or page-table state. The callback boundary allows another backend
to provide context/object/submit/wait/cleanup operations later; Haswell command
encoding remains explicitly generation-specific.

Added `make gpu-submission-test`. Simulated callback tests cover malformed
commands and lengths, invalid arguments, successful multi-submit completion,
exec failure, timeout, upload failure, invalid GPU VA, partial context/GEM
creation, GEM close failure, and context destruction attempts. The sanitizer
test covers the same software state machine; these tests are not physical GPU
evidence.

The existing `gpu-memory-test` was audited and its command-buffer encoding and
length were aligned with the same two-dword sequence. Hardware experiments
found that the previous 4-byte length returned `EINVAL`; the original pinned
GPU VAs `0x00100000` and `0x00200000` returned `ENOSPC`. Updated the fixed
addresses to `0x10000000` and `0x10100000`, which bound successfully.

**Hardware execution evidence (separate from software tests):**

- The final sequential `IDW_GPU_SUBMIT_COUNT=3 make gpu-test` passed on
  `/dev/dri/renderD128`, Haswell `8086:0412`, with i915 as owner.
- Three no-op render batches were submitted and all three GEM_WAIT calls
  returned success. The test reported context ID 1 and last returned GPU VA
  `0x011a66000`, aligned to 4 KiB. GEM handles and the context were closed.
- `make gpu-memory-test` passed on the same device: i915 bound objects at
  `0x10000000` / `0x10100000`, completed initial submissions, processed
  DONTNEED/WILLNEED, and completed rebinding/submission in the fresh context.
  Backing purge was not observed on this run.

This is genuine i915-assisted execution and completion evidence for the
harmless test batch. It does not show IDW-owned execution, page tables,
translation, DMA, scheduling, residency, forced eviction, or ownership. i915
remains the physical GPU owner.

Final checks passed: `make test`, `make kernel`, `make gpu-submission-test`,
GCC `-fanalyzer` on submission/integration code, and ASan/UBSan with leak
detection on submission failure/timeout/cleanup tests. Existing libdrm header
`-Wpedantic` warnings remain. `/dev/idw` integration checks skipped because
the module was not loaded.
