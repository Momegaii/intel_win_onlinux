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

## Integrated memory-lifecycle core

Added `include/idw/memory_manager.hpp` and `src/memory_manager.cpp` to compose
the existing GPU-VA interval allocator with backend-managed backing resources,
binding/unbinding, invalidation, submissions, and destruction. The manager
rounds backing sizes to pages, reserves GPU VA before binding, and releases the
reservation only after unbind plus invalidation complete. A failed operation
retains enough state for explicit binding recovery. A backend-returned VA that
does not match the reservation is treated as a consistency fault; new binds and
submissions are blocked until its mapping is removed and invalidated.

Each submitted resource receives a manager-held reference until a successful
wait or explicit backend quiescence recovery. Timeouts and uncertain wait
errors retain the reference, so unbind/destroy remain busy. Backing destruction
failure likewise leaves the allocation tracked and retryable. The manager
requires caller-side serialization. It is backend-neutral and currently
exercised only with a simulated callback backend; it is not wired to the i915
GEM/context path and does not implement native DMA, PPGTT, residency, or
eviction. Existing physical GPU evidence remains i915-assisted.

Added `make memory-manager-test` to the regular test suite. Tests cover rounded
allocation size, complete allocate/bind/submit/wait/unbind/invalidate/destroy
lifecycle, reference-protected destruction, failed backing create, failed bind
rollback, failed unbind and invalidation retry, unexpected backend VA and
recovery, multiple outstanding tickets, wait timeout, failed and successful
quiescence, malformed zero-fence recovery, backing-destroy retry, invalid
arguments, and shutdown guards. The
first compile attempt exposed that `gpuva_model.h` lacked C++ linkage guards; those
were added so the manager links to the existing C implementation rather than
duplicating it.

Validation completed:

- `make memory-manager-test` passed with strict C11/C++17 warnings-as-errors.
- `make test` passed, including the new lifecycle tests and existing i915 GEM
  smoke tests. `/dev/idw`-dependent tests skipped because `/dev/idw` is absent.
- `make kernel` passed; no kernel modules were loaded.
- AddressSanitizer and UndefinedBehaviorSanitizer with leak detection passed
  for the new manager and GPU-VA allocator tests.
- GCC `-fanalyzer` passed for `memory_manager.cpp` and `gpuva_model.c`.
- `git diff --check` passed. The existing external libdrm `-Wpedantic` warnings
  remain unrelated to the new manager.

The i915 GEM smoke test in `make test` is hardware-backed, but the integrated
memory-manager tests remain simulated. No hardware run exercised the new
manager; i915 remains the only physical GPU owner.

## i915-assisted integrated memory-manager backend

Added `I915AssistedMemoryBackend` using public DRM/i915 GEM, context,
execbuffer, and wait ioctls. `create_backing` creates/tracks a GEM object;
`bind` creates a per-resource context and submits/waits a harmless two-dword
Haswell batch with the backing object pinned at the manager's reserved GPU VA.
The i915 context owns that VM binding. `submit` creates a per-submission batch
and includes both the managed resource object and batch in the context
execbuffer; `wait` maps the manager ticket to that batch's GEM_WAIT; unbind
destroys the per-resource context, which releases the i915 VM binding, before
invalidation is acknowledged to the manager. Only then can VA reuse and backing
destruction proceed. This is intentionally a replaceable assisted backend,
not IDW PPGTT or DMA.

Callback failure semantics were extended for operations that may have taken
effect despite returning an error. A bind callback returning `-EUCLEAN` with
the actual VA retains the reservation and requires `recover_binding`. A submit
callback returning `-EUCLEAN` retains its ticket/resource reference until a
wait or quiesce proves safe. The i915 backend registers submitted batch handles
as fences immediately after successful execbuffer submission, before fallible
bookkeeping, so a tracking error cannot drop an in-flight resource reference.

Added `tests/i915-memory-backend-test.cpp` with simulated ioctl behavior.
Coverage includes multiple tracked GEM objects/contexts, resource-inclusive
execbuffers, exact Haswell no-op payload validation, held references across a
failed wait, infinite-wait quiescence recovery, context-destroy failure/retry,
partial/mismatched GPU-VA recovery, GEM-close failure/retry, and final
GEM/context/fence cleanup. These are simulated ioctl tests, not physical
hardware evidence.

Added explicit `gpu-memory-manager-test` / `make gpu-memory-manager-test`.
Physical result on 2026-11-20: PASS on Haswell `8086:0412` with i915 bound.
Three managed resources were allocated, assigned VAs `0x10000000`,
`0x10001000`, and `0x10002000`, pinned through i915 context execbuffers,
included in further harmless no-op submissions, and each `GEM_WAIT` completed.
The manager then destroyed the contexts (releasing their i915-managed VM
bindings) and closed all GEM backing. No resource data was dereferenced by the
no-op command. The physical test did not inject timeouts, wait errors, or
invalidation failures; those recovery paths were exercised only by simulated
callback/ioctl tests. i915 remains the physical GPU owner.

Final validation for this integration passed: `make test`, `make kernel`,
`make i915-memory-backend-test`, `make memory-manager-test`, both manager
AddressSanitizer/UndefinedBehaviorSanitizer runs with leak detection, GCC
`-fanalyzer` on the allocator/manager/i915 adapter, shell syntax checking, and
`git diff --check`. The explicit `make gpu-memory-manager-test` hardware run
passed again after the final code changes. `/dev/idw`-dependent tests skipped;
no kernel module was loaded. The build retains the existing libdrm header
`-Wpedantic` warnings.

## IDW 0.9.0 — GPU resource read/write verification

### Haswell command-source verification

Before any physical write submission, verified the encoding against Intel
Corporation's *Intel Open Source Graphics Programmer's Reference Manual (PRM)
for the 2013 Intel Core Processor Family, Volume 2b: Command Reference:
Instructions (Command Opcodes) (Haswell)*, dated 2014-01-21. The
`MI_STORE_DATA_IMM` RenderCS entry is on printed pages 236-238:

- DWord length is 2 (total command size is four dwords): header, reserved
  dword, 32-bit graphics address, and immediate data.
- Header `0x10000002` encodes MI opcode `20h`, a DWord-sized command, and leaves
  Use Global GTT clear; the PRM says that selects PPGTT address translation.
- The target field is address bits 31:2, with DWord alignment required.
- `MI_BATCH_BUFFER_END` is documented for RenderCS on printed page 194. The
  batch appends it after the store and a `MI_NOOP` pad, making the 24-byte
  batch length 8-byte aligned.

The Intel manual PDF was retrieved from the [Intel graphics PRM index mirror]
(https://kiwitree.net/~lina/intel-gfx-docs/prm/), whose index identifies itself
as mirroring Intel's official PDFs; the PDF itself carries Intel Corporation
copyright and metadata. Intel's [official PRM landing page]
(https://www.intel.com/content/www/us/en/docs/graphics-for-linux/developer-reference/1-0/overview.html)
no longer lists the archived Haswell document.

The PRM also notes that `MI_STORE_DATA_IMM` initiates the write and that its
completion is eventual rather than synchronized with following command
execution. Therefore the test waits for GEM batch completion and then polls
CPU GEM_PREAD at 1 ms intervals for up to five seconds; it does not infer store
visibility solely from GEM_WAIT.

### Implementation and evidence

- Extended the existing `MemoryManager` and i915 backend rather than adding a
  parallel resource/context subsystem. The manager now validates aligned
  in-bounds dword CPU accesses and managed write submissions, retaining its
  submission reference until wait or quiescence. The i915 adapter emits only
  the documented Gen7.5 command, pins the managed GEM object at its context VA,
  submits through the existing context, and uses public GEM_PWRITE/PREAD and
  GEM_WAIT ioctls.
- Added command construction/validation for the fixed 24-byte Haswell batch,
  including malformed header/reserved/end/padding, alignment, address-width,
  and length checks. The simulated i915 ioctl test emulates only that command's
  write into the named resource and covers bounds rejection, failed exec,
  failed PREAD, timed-out wait followed by quiescence, held references, and
  cleanup.
- Added opt-in `make gpu-resource-write-test` /
  `idwctl gpu-resource-write-test`, restricted to Haswell `8086:0412` with i915
  bound. It initializes a resource dword to `0x13579bdf`, submits a write of
  `0x2468ace0` at resource offset 16, waits, reads it back through PREAD, then
  unbinds and destroys the manager-owned resource.
- Physical result on 2026-11-20: **PASS** on Haswell `8086:0412` with i915.
  The manager-selected resource GPU VA was `0x10000000`; the GPU wrote offset
  16 (GPU VA `0x10000010`). CPU readback observed exactly `0x2468ace0`, changed
  from the initialized `0x13579bdf`. The test cleaned up the context, GEM
  backing, and DRM fd. This proves an i915-assisted GPU write/readback for this
  resource, not IDW-owned DMA, PPGTT, translation, scheduling, or execution.

Validation completed:

- `make i915-memory-backend-test gpu-submission-test memory-manager-test`
  passed, including malformed encoding and simulated failure/recovery cases.
- `bash -n tests/gpu-resource-write-smoke.sh` passed.
- `make gpu-resource-write-test` passed on physical Haswell/i915 with the exact
  readback above.
- `make test` passed. Tests requiring `/dev/idw` skipped because that device
  node is absent. Existing `-Wpedantic` warnings remain confined to system
  libdrm headers.
- AddressSanitizer and UndefinedBehaviorSanitizer with leak detection passed
  for the simulated i915-memory-backend and MemoryManager test binaries.
- Final `git diff --check` passed. No kernel module was loaded and no i915
  ownership or binding change was made.

## Stage 2 — native DMA provider progress

Added an internal kernel DMA page provider in `include/idw/dma_memory.h` and
`kernel/idw_dma_memory.c`. It accepts only a PCI device whose current driver's
module matches the explicitly supplied owner, requires the owner to have
configured both streaming and coherent masks for the Haswell 40-bit DMA range,
allocates zeroed 4 KiB pages with `dma_alloc_coherent`, validates CPU/DMA
alignment and the complete DMA-address range, and returns device-cookie,
generation, and per-page lifetime-cookie metadata. Free validates the exact
scope and descriptor; provider teardown frees any remaining pages. Provider
init is intended for the owning driver's probe, and teardown for remove after
the hardware is quiesced. This is separate from the CPU system-memory and
i915-assisted GEM paths.

The disposable test miniport also includes a negative check that provider
initialization rejects a missing device scope. It is compiled into the test
module but has not been executed because no kernel module was loaded.

Validation: `make test`, `make kernel`, and a warning-enabled (`W=1`) Kbuild
compiled and linked the modules against the running kernel headers. No module
was loaded and no DMA allocation or map was executed. The current IDW module
has no PCI binding/owned GPU device, while Haswell remains bound to i915.
Therefore the provider's real device allocation and DMA-address result remain
unverified and Stage 2 is **in progress, not complete**. It is not wired into
the CPU PPGTT model or the native DMA contract yet. Stage 3 must wait for real
Stage 2 device-scoped allocations and hardware visibility evidence.

### Stage 2 continuation check

Rechecked the physical device inventory before attempting any native DMA
operation. The only display device is Haswell `8086:0412` at `0000:00:02.0`,
still bound to i915; there is no separate IDW-owned GPU. This process runs
without root privileges, so the kernel test modules were not loaded. No PCI
driver binding was changed and no DMA allocation or mapping was attempted.

Validation in this continuation: `make test` passed, including available
i915-backed GEM smoke/lifecycle checks; checks requiring `/dev/idw` skipped
because the module/device is absent. Both the normal kernel module build and
warning-enabled (`W=1`) Kbuild passed. These checks do not prove native DMA.
Stage 2 remains blocked on a safely IDW-owned compatible device; Stage 3 must
not start until Stage 2's real DMA allocation/mapping and cleanup evidence is
available.

### QEMU virtual PCI DMA harness

Implemented an opt-in guest test using QEMU's upstream EDU PCI device
(`1234:11e8`), whose emulated bus-master DMA engine transfers between guest RAM
and its internal 4 KiB buffer. The guest driver in
`kernel/idw_edu_dma_test.c` binds only to EDU and calls the existing IDW
`dma_alloc_coherent()` provider. It checks matching PCI-driver ownership,
rejects a 32-bit mask, configures the 40-bit mask, checks page and DMA-address
alignment/range, rejects malformed scope/length/address/cookie descriptors,
and runs 32 exact 4 KiB device-DMA read-then-write round trips with DMA
ordering barriers. It retains one page so driver removal exercises the
provider's outstanding-allocation cleanup.

The test-runner builds modules and a matching-kernel initramfs, boots QEMU with
TCG only, and uses `isa-debug-exit` to distinguish successful remove cleanup
from failure or timeout. A `--build-only` mode builds and inspects the guest
artifacts without claiming DMA execution. Corrected the DMA provider's owner
module self-reference: pinning the calling PCI driver's module from its own
probe would prevent unload and therefore prevent the remove callback that
releases the provider.

Build validation completed: normal kernel-module build, warning-enabled
(`W=1`) Kbuild, `make test`, guest-init static compilation, shell syntax, and
build-only initramfs creation all passed. Module metadata matched the guest
kernel, and the initramfs contains both modules and `/init`. The host has no
VMX/SVM flags or `/dev/kvm`; the test therefore ran in TCG mode after the
user-approved installation of Arch package `qemu-system-x86`.

**Executed hardware-backed virtual test:** `make qemu-dma-test` passed on QEMU
11.1.3, TCG, guest kernel `7.3.5-arch1-1`, with QEMU EDU `1234:11e8` configured
for a 40-bit DMA mask. The guest driver verified the bound-owner check,
rejected a 32-bit mask, then initialized the existing provider with its 40-bit
mask. The log shows a 4 KiB transfer between coherent DMA addresses
`0x303c000` and `0x303d000`, starting with dword `0x833879b9`; all 4096 bytes
matched after the EDU device read from guest memory into its internal DMA
buffer and then wrote that data to the second guest page. The full 32-cycle
read/write loop passed. Invalid length, scope, DMA-address, and cookie
descriptors were rejected; a page deliberately retained through test completion
was freed during PCI remove (`outstanding provider pages=yes result=0`).
QEMU reported the pass marker and debug-exit status 33. The full successful
serial output is preserved at
`tests/qemu-dma/evidence-2026-11-20-tcg.log` (SHA-256
`ea8de8d1ba5c1c4c8d92e81a5c3d6cbec5e7b9f1adef25dd5efea71776109253`).

This is genuine DMA by QEMU's emulated PCI device through the guest Linux DMA
API, not a software-only simulation. It validates the provider and lifecycle
in the virtual environment only; it is **not** physical Haswell DMA proof.
Stage 2's QEMU validation gate passed, while physical native DMA remains
unproven and the overall native-DMA stage remains in progress. The host GPU
`8086:0412` remained bound to i915 throughout; no host PCI binding or ownership
was changed.

### DMA-backed page-table storage continuation

Added a storage-only page-table memory adapter between the Haswell Gen7.5 PPGTT
software model and a DMA page provider. `include/idw/pt_dma_adapter.h` and
`src/pt_dma_adapter.c` validate zeroed, page-aligned coherent or streaming
pages, 40-bit DMA range, device/generation scope, allocation cookies,
synchronization bounds, and explicit release before teardown. The adapter
advertises `IDW_PT_BACKEND_PAGE_TABLE_ONLY`; the complete native-DMA contract
rejects that partial profile. `kernel/idw_dma_pt_adapter.c` binds it to the
existing ownership-checked kernel provider, and `kernel/pt_dma_adapter_shared.c`
plus `kernel/page_table_memory_shared.c` compile the portable code with kernel
headers.

The QEMU EDU test module is now a composite (`idw_edu_dma_test_main.c`,
`idw_dma_pt_adapter.c`, and the two shared units). Its probe allocates page-table
pages through the adapter, fills a Gen7.5 page table, synchronizes it for the
device, copies it to a second DMA page with EDU, synchronizes for the CPU, and
verifies the exact bytes and first/last PTE layout. It also rejects malformed
page descriptors and unsupported resource mapping, then releases every page and
checks the adapter is no longer busy before the existing 32-cycle DMA test.

Added `make pt-dma-adapter-test` with a simulated provider. It covers adapter
validation, allocation-failure rollback, provider-reported scope/range/
alignment/cookie/type/size errors, synchronization, release, teardown, backend
capability rejection, and PPGTT model integration including deferred table
reclamation. The contract test now also rejects a page whose 40-bit DMA range
overflows or whose length is not page-aligned.

Validation in this continuation: `make contract-test abi-test
backend-contract-test ppgtt-model-test pt-dma-adapter-test
memory-manager-test gpu-submission-test` passed; ASan/UBSan passed for the
adapter, PPGTT model, and page-table contract tests; `make kernel CC=gcc` and
`make kernel CC=gcc W=1` built all three modules. The full `make test` compiled
all userspace targets after installing Debian `libdrm-dev` and `pkg-config`, but
its final `tests/smoke.sh` step failed because this container has no Intel DRM
render node. The QEMU guest test was not executed in that continuation; the
follow-up run below covers page-table storage. No hardware page-table
publication, GPU invalidation, VM activation, resource mapping, MMIO, physical
Haswell DMA, or independent GPU ownership is claimed.

### QEMU EDU page-table DMA execution

Installed QEMU 10.0.13 and a matching Debian `6.12.117+deb13-amd64` kernel and
header tree in the test container, then executed the previously unrun
page-table path:

```sh
KDIR=/lib/modules/6.12.117+deb13-amd64/build \
IDW_QEMU_KERNEL=/boot/vmlinuz-6.12.117+deb13-amd64 \
make qemu-dma-test
```

The first attempts timed out with no guest serial output because `timeout`
runs QEMU in a separate process group and the `stdio` serial backend stopped on
`SIGTTIN` while reading the terminal. `tests/qemu-dma/run.sh` now redirects
QEMU stdin from `/dev/null`; the test then passed from `make`.

The guest allocated page-table pages through the DMA adapter, copied a
populated Gen7.5 page table to a second DMA page with EDU, and verified the
exact bytes and first/last PTE values:

```text
IDW_QEMU_DMA: page-table DMA layout verified bytes=4096 first=00000ab1 last=003ffab1
IDW_QEMU_DMA: page-table malformed descriptors and unsupported mapping rejected
IDW_QEMU_DMA: page-table adapter cleanup verified
IDW_QEMU_DMA: 32 device DMA read/write cycles verified
IDW_QEMU_DMA: remove freed outstanding provider pages=yes result=0
IDW_QEMU_DMA: PASS: probe, page-table DMA, 32 DMA cycles, cleanup and remove completed
QEMU exit status: 33
```

The full serial output is preserved at
`tests/qemu-dma/evidence-2026-11-20-tcg-page-table.log` (SHA-256
`e549ebbf7664bdfff05d8ea259efd2df1ace950cb67ab259c47c57a4b198d6da`). This
closes the previously open QEMU page-table evidence gap. It remains QEMU
virtual-device DMA evidence, not physical Haswell DMA or hardware PPGTT; Stage
2 remains in progress and Stage 3 remains not started.
