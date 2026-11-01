# DOCA_MEMOS splitting PoC

Branch: `codex/memos-split-poc`, rebased onto `benlwalker/doca_kv` at
`00df0974fd27b0536e0ff1a058e88b5bb6e18256`.

## Behavior

The backend expands each logical READ/WRITE descriptor into contiguous payload
slices. Each slice becomes an ordinary DOCA KV task, using the existing task
queue, submission backpressure and completion/error handling. There are no
payload copies, stored headers, manifests or internal metadata validation.

- Chunk 0 keeps the original resolved key, K.
- Continuation i uses BLAKE2b-128 of `"nixl-memos-chunk-v1" || key_length_u8 || K || i_le64`.
  This reuses the existing libb2 dependency and produces a 16-byte key.
- At initialization, discover the namespace maximum value length (MXVL) through
  DOCA and the kernel single-command transfer limit from the NVMe queue's
  `max_hw_sectors_kb`. Default chunk bytes are the smaller of these limits.
  Standard `/dev/ngXnY` and `/dev/nvmeXnY` names and resolvable aliases are
  supported; discovery uses `/sys/block/nvmeXnY/queue/max_hw_sectors_kb`.
- Optional backend initialization parameter `chunk_size` (advertised default `0`,
  meaning automatic discovery) overrides chunk bytes when nonzero,
  capped by both discovered limits. If the kernel limit is unavailable, an
  explicit override is required and is capped by MXVL; the caller must ensure
  it fits the actual transport. A zero namespace maximum is rejected.
  NIXLBench submits logical objects without specifying chunk size.
- EXIST checks only K using the upstream backend's device query.
- READ/WRITE succeed only when all children succeed. Missing reads return the
  upstream `NIXL_ERR_BACKEND` for both split and unsplit objects, whether some
  or all chunks are absent. Other child failures also fail the logical request.
  Upstream removed `ignore_read_not_found` and `query_mem_mode`; this patch does
  not reintroduce them or change NIXLBench.

All participants must use the same effective chunk size, supported by their
interfaces, and data must have been written with that geometry. The backend does
not handle unaligned geometry, validate object generations/lengths, roll back partial writes, or clean up stale
continuations after a size change. Concurrent updates may mix chunks. EXIST can
succeed while a continuation is absent. A failed read may already have modified
part of the destination. Use freshly populated test values; existing unsplit
large values are not migrated.

## Local checks completed

After rebasing onto `00df0974`, the actual backend/progress sources compiled
against the mocks and all **82 tests passed**. Upstream parameter parsing,
key encoding, size_t counters, locking and cancellation handling are retained.
Split-object cancellation tests cover READ and WRITE in both progress modes:
two chunks submitted, three pending, release refused until submitted I/O drains,
unsubmitted chunks left untouched, and the same handle successfully reused.
The advertised `chunk_size=0` default is also checked against omitted-option
behavior. The rebased tree has not been rerun on Daria. The automatic-sizing
Daria run
(job 116604, automatic 8 MiB and manual 4 MiB round trips plus both benchmark
progress modes) validated pre-rebase commit `60f85e6c`.

Compiled the actual backend and progress-engine sources against the repository's
DOCA mocks, compatible DOCA KV public headers, GoogleTest 1.17.0 and libb2 on
macOS. Before rebasing, all **74 tests passed**. Tests cover multi-chunk byte-for-byte round trips
and overwrites, a short final chunk, missing continuations with first-key EXIST,
failed child submission, device maximum splitting, request reuse and both
progress modes. The mock notification handle initialization was made portable
using `{}` instead of an integer literal. Coverage verifies matching
split/unsplit status for all-missing and partially missing objects, and hard
failures in both progress modes. After rebasing, obsolete ignore-option tests
were removed to follow upstream; full-width continuation-key validation was
added for namespaces with smaller key limits. Transfer-limit tests cover sysfs discovery, limits
above 8 MiB, smaller and larger overrides, namespace capping, missing/malformed
sysfs data, and requiring an override when automatic discovery is unavailable.

These status-policy checks were run locally after the live checks below; the
live binaries predate the final missing-read policy correction and automatic
kernel-limit discovery; those final changes have local mock coverage only.

These checks use mocks. The separate live check below uses the real Linux plugin
and CMX device.

## Live NIXL smoke test (2026-11-01)

Passed on `daria04`, Slurm job **116586**, exit **0:0**. A standalone client using
the public NIXL C++ API issued one 20 MiB WRITE, checked first-key EXIST, cleared
the buffer, then issued one READ. All **20,971,520 bytes** matched the generated
pattern. Both transfers used three physical chunks: **8 + 8 + 4 MiB**. Observed
write/read completion times were 3.180/9.079 ms; these single operations are not
a performance benchmark.

The device was `/dev/ng10n1`, NUMA node 1, with an advertised 8 MiB maximum value
length. The buffer used explicit hugepages and the progress thread was disabled.
The actual backend and NIXL core were built against DOCA SDK **3.6.0063** into a
private installation; process mappings verified that those libraries were loaded.
The source snapshot matched its recorded hashes. Initiator/target service images
were not changed or reverified. Temporary device ACL changes were restored.

Full test source, build recipe, logs, hashes and results are retained under
`/hpc/newhome/yaniv/nixl-memos-split-daria04-20261101`, with a local copy under
`/Users/yaniv/Downloads/nixl-memos-split-daria04-20261101` (`RESULTS.md`). This live
check covers the positive path only; failure propagation remains mock-tested.

## Live NIXLBench contention validation (2026-11-01)

Passed on `daria04`, job **116591**, exit **0:0**, with both progress-thread modes.
Each invocation used eight **36 MiB** logical objects (4.5 times the 8 MiB limit),
four workers, batch two and pipeline depth one. Each request expanded into ten
physical tasks, sharing a pool of eight. Actual task-pool exhaustion and retries
were observed, followed by successful completion and request reuse.

A separate test-only NIXLBench hook filled object-specific, offset-varying
patterns, poisoned each read buffer before posting and compared every payload
word after completion. All **272 logical reads / 9.5625 GiB per mode** matched;
each mode completed 40 stores and 1,360 physical retrieves without backend errors
or short reads. The backend binary was unchanged. Validation and DEBUG logging
make these correctness results unsuitable as performance measurements.

The test hook is retained outside this PoC in
`/Users/yaniv/Downloads/nixl-memos-split-contention-daria04-20261101`, including
`RESULTS.md`, `REVIEW.md`, raw evidence and the validation patch. Remote artifacts
are under `/hpc/newhome/yaniv/nixl-memos-split-contention-daria04-20261101`.
Review found no blocking defect within the PoC contract; it corrected stale
comments and help text without changing backend logic. Live missing-child and
cancellation injection remain untested.

## Simple live test plan

Use NIXLBench: direct FIO access to NVMe-KV bypasses this NIXL backend and cannot
test this implementation. An existing FIO setup remains useful as a control for
physical, unsplit transfers.

### Build in a compatible Linux SDK environment

Use a DOCA KV SDK compatible with this branch, plus the repository's normal build
dependencies (including libb2 and GoogleTest). From this worktree:

```bash
meson setup build-poc -Denable_plugins=DOCA_MEMOS -Dwith_trace=false \
  --prefix="$PWD/install-poc" --libdir=lib
meson compile -C build-poc
meson test -C build-poc --suite doca_memos --print-errorlogs
meson install -C build-poc

meson setup buildbench-poc benchmark/nixlbench \
  -Dnixl_path="$PWD/install-poc" --libdir=lib
meson compile -C buildbench-poc
```

These Linux build commands are a plan and have not been run locally. Ensure the
benchmark loads this installation's library and plugin, rather than the existing
system plugin:

```bash
export LD_LIBRARY_PATH="$PWD/install-poc/lib:${LD_LIBRARY_PATH:-}"
export NIXL_PLUGIN_DIR="$PWD/install-poc/lib/plugins"
```

### Smoke test

Use the approved test namespace/device and the usual reachable etcd endpoint.
Hugepages must already be available. Replace the device, NGUID and endpoint
variables with the test environment's values. Run one size per invocation so the
READ path repopulates values using the same geometry before reading them.

```bash
NIXL_LOG_LEVEL=DEBUG ./buildbench-poc/nixlbench \
  --etcd_endpoints="$ETCD_ENDPOINT" \
  --backend=DOCA_MEMOS --op_type=READ \
  --doca_memos_device_name="$KV_DEVICE" --doca_memos_nguid="$NGUID" \
  --total_buffer_size=67108864 \
  --start_block_size=16777216 --max_block_size=16777216 \
  --start_batch_size=2 --max_batch_size=2 --num_threads=1 \
  --storage_enable_direct=true --use_hugepages=true \
  --num_iter=10 --warmup_iter=1 --large_blk_iter_ftr=1 --enable_pt=false
```

The existing READ path first writes the keys, retrieves them once, then performs
warmup and timed reads. Require successful prepopulation, retrieval and benchmark
completion. Its verification messages check operation success, **not payload
equality**; `--check_consistency` is unsupported for DOCA_MEMOS. Byte comparison is
covered by the mock round-trip tests above; a live byte comparison would require
a small dedicated NIXL client or a separate benchmark verification change.

Run the following fixed block sizes, with both `--enable_pt=false` and `true`:

| Logical bytes | Size | Physical chunks per object at 8 MiB |
| ---: | ---: | ---: |
| 1048576 | 1 MiB | 1 |
| 8388608 | 8 MiB | 1 |
| 16777216 | 16 MiB | 2 |
| 20971520 | 20 MiB | 3 (8 + 8 + 4 MiB) |
| 25165824 | 24 MiB | 3 |

For batch 2, the debug preparation message should show twice the per-object
physical task count. Also repeat 24 MiB with `--doca_memos_num_tasks=2` to
exercise physical-task backpressure. Compare 1/8 MiB against the unchanged build
as a smoke check for the unsplit path; these short runs are not performance
qualification.

Missing-child and partial-write error tests are already deterministic in the
mock suite. Avoid deleting production keys merely to reproduce these failures.
