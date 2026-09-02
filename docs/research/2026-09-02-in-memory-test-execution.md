# Running the test suite from memory: what the disk is actually doing (2026-09-02)

Companion to `2026-08-19-gate-performance-audit.md` and
`2026-08-21-gate-performance-measured.md`. Like the audit, this note was produced
in a read-only remote session with no LLVM and no build tree: every figure below
is either quoted from an in-tree comment, from the two earlier notes, or derived
from them, and is marked as such. Nothing was built or run. The question it
answers is the operator's: *the suite is slow and needs a fast SSD or its
artifacts cannot be written fast enough — can we build and execute the tests in
memory, or execute each test program as it is built and discard it?*

**Headline.** The suite does not need a fast SSD to *run* tests. It needs a fast
SSD to *store* them: the per-test executable is written once, then hardlinked
into the AOT bincache under two or three keys with two `fsync`s and a `rename`
per store, and that durability dance is what a 7200 RPM disk cannot keep up with
(~5 durable creates/s, `CLAUDE.md`; "each cold-test cache store pays 4 fsyncs
≈260ms", `scripts/lane_setup.sh:87-88`). Meanwhile **91% of gates are cold**
(`measured.md §1`) because the cache key includes the compiler's own identity,
so on the traffic that dominates, the store is paid and the hits never come. The
runner already contains the "execute as built then discard" mode the operator is
asking for — `BEZEL_AOT_CACHE=0` — and it has twice been measured or observed
faster than a cold gate with the cache on (`measured.md §3`: 634s vs 1465s ASan;
`scripts/gate.sh:599-601`: the operator-remembered `BEZEL_AOT_CACHE=0` pair
"bought 4 attempt-1 greens out of 6 oracles versus roughly 0 of 8"). The
per-test *transient* footprint, by contrast, is a few megabytes and is an
excellent fit for RAM. The build tree and the bincache are not.

Nothing here changes what a green gate proves.

## 1. Where a test's bytes go today

Per `runAot` test on the cold, split path (`tests/support/aot_support.hpp`,
line numbers at HEAD 09c1803):

| Step | Filesystem touch | Where |
|---|---|---|
| workdir | `mkdtemp(scratchBase()/aotXXXXXX)` | `:289-298` |
| source | `ofstream prog.bzl` — only because `Driver::compileFile` takes a path (`src/driver/Driver.cpp:448`); the harness already holds the bytes | `:1896-1901` |
| link token | `open`+`flock` on `/var/tmp/bezel-scratch/aot-link-tokens/tok.N` (hardcoded path) | `:489-538` |
| compile child | `fork()`, stdout+stderr → `cc.err` file, `Driver::compileFile` in-process | `:651-732` |
| object | `raw_fd_ostream` straight to `prog.o`, no temp, no fsync | `src/codegen/LlvmBackend.cpp:117-130` |
| object hash | full read-back of `prog.o` for the K2 key | `:1862` |
| link child | second `fork()`, `std::system("clang … -o exe … libbezel_runtime.a")` → `sh` → `clang` → system `ld` | `:808-819`, `Driver.cpp:283-344` |
| run child | `fork()`, stdout → `out.txt`, stderr → `run.err`/`san.txt` (files, not pipes), `execv(exe)` | `:243-285`, `:1971-1973` |
| **store ×3** | per store: `create_hard_link` (fallback `copy_file`), `chmod`, `open`+**`fsync`**, `rename`, `open(dir)`+**`fsync`** | `:1591-1639`, `:1461-1479` |
| cleanup | `remove_all(workdir)` | `:301-312` |

So a cold miss is six processes, three files of substance (`prog.o`, `exe`, and
the source), four capture files, three hardlinks, three renames and **six
`fsync`s**. A warm KE hit is one hardlink, one `chmod`, one `utimensat` (the
LRU touch, `:1800-1804`) and one exec. Nothing in the hot path needs durability
except the store.

Sizes (derived in `measured.md §3` and `scratch_dir.hpp:102-106`, not
re-measured): ASan executable ≈ 2.0–2.3 MB, plain ≈ 0.64 MB, object ≈ 0.1–0.2
MB. A full ASan pass stores ~30 GiB (`aot_support.hpp:1659-1667`); the store's
cap is 64 GiB with no eviction below the cap (`:1669-1673`). The live transient
set is one workdir per shard: **16 shards × ~3 MB ≈ 50 MB**.

Two further facts shape any plan:

- **The workdir and the store are welded together by construction.**
  `scratchBase()` defaults to the cache root's sibling (`scratch_dir.hpp:112-123`)
  so that the hardlink store is free. Put the workdir on a different filesystem
  and every store and hit silently becomes a byte copy — the shape that cost
  34–57% of the shard phase when the workdir sat on the spinning disk
  (`measured.md §3`). On tmpfs the copy is a few hundred microseconds of
  memcpy, not a disk round-trip, but the fsyncs still land on the store's disk.
- **Roughly 171 `tests/module` files bypass the harness** (`measured.md §5`):
  they compile in-process without a fork, `std::system("clang++ …")`, and run
  via `system()` from `wtScratchDir()` (e.g.
  `tests/codegen/json_options_687_test.cpp:92-140`). Across the tree, 372 test
  files call `system()`/`popen()` and 265 call `temp_directory_path()`; all of
  them resolve through `scratchBase()` or `TMPDIR`, which `test_main.cpp:159-178`
  points at `scratchBase()` when unset.

## 2. What the disk speed actually gates

- Cold-vs-warm is the cost, and cold is the norm. Medians since 2026-08-15:
  plain shard phase 555s, ASan 1465s, of a ~43 min cycle; 252 of 278 gates
  cold (`measured.md §1`). The key hashes `libbezel_core.a` (or the test binary)
  plus `libbezel_runtime.a` plus the toolchain (`aot_support.hpp:1060-1173`), so
  any compiler-source change misses everything by design.
- The store, not the compile, is the disk-bound part. Cache disabled
  (compile everything, store nothing): 634s. Cold with store: 1465s. The 831s
  gap "is the store path" (`measured.md §3`). That measurement predates the
  colocation fix of 2026-08-21 (which removed the byte copies and halved the
  stored bytes); the post-fix full-suite cold figure has not been recorded in
  either note and should be read off `gate-timings.jsonl` (see §6). Even with
  colocation, a cold miss pays ~6 fsyncs, i.e. ~66 ms on the NVMe (11 ms each,
  `CLAUDE.md`) and ~390 ms on sda3 (65 ms each) — the latter is the "5.7s/test,
  1800s ceiling" failure mode of `lane_setup.sh:92-95`.
- The cache's value is real but narrow: retries within a cycle (a killed
  95%-complete attempt leaves the store ~95% warm, so a retry is ~310/500s
  instead of a full re-run, `audit.md`), the ~9% of gates that are test-only
  changes, and lane iteration on tests. It does **not** help plain → ASan
  within a cycle (the `sa=` bit is in every key, `aot_support.hpp:1195`).

## 3. What already runs from RAM

`tests/run_parallel.sh:88,165` puts the shard log directory and every shard's
`TMPDIR` under `${TMPDIR:-/tmp}` — the 16 GB tmpfs — and `test_main.cpp:177`
sets `TMPDIR` with `overwrite=0`, so the shard keeps it. `measured.md §5` names
the inversion: the ~171 harness-bypassing module tests write and execute their
programs from RAM today, while the `runAot` tests that are the suite's bulk use
the NVMe (or, before 2026-08-21, the spinning disk). The tmpfs use has not
caused an incident since the leak fix; it is simply unpriced.

## 4. The tmpfs incidents, re-read

Every recorded RAM incident is a *parking* or *leak* failure, not a transient-use
failure:

- 2026-07-29: per-shard AOT **caches** rooted under `${TMPDIR:-/tmp}` filled the
  16 GB tmpfs (`scripts/dev_heavy.sh:20-33`). A cache is durable state; it does
  not belong in RAM.
- 2026-08-24: 2,643 leaked `wt-*` dirs / 12 GB under `/tmp` from four files that
  hardcoded it, plus 148,949 dirs / 41 GB under `/var/tmp` — no test removed its
  scratch (`scratch_dir.hpp:12-23`). Fixed structurally by `wtScratchDir()`
  at-exit removal plus the age-gated reaper.
- `native_test_harness.hpp:138-141`: 16 shards each rebuilding ~100 MB of ASan
  runtime objects into RAM. That is a per-shard rebuild of shared durable state;
  the rt-objs cache (8.7 GB) is disk-shaped.

The audit's Tier 3 already records the gap: "tmpfs workdirs — unpriced
alternative to F4; CLAUDE.md's /tmp warning is about parking, not transient
files, but nobody has measured it. Open." This note prices it structurally;
§6 gives the commands to price it numerically.

## 5. Options

### O1 — Per-test workdir on a dedicated, size-capped tmpfs (zero harness code)

What: mount a tmpfs sized for the transient set, e.g. 2–4 GB, at a path of its
own (`/mnt/ram/bezel-aot-harness`, *not* `/tmp` and not `/dev/shm` — both are
shared with other tenants and carry the 12,774 MB per-user quota that wedged the
2026-08-24 sessions), and point `BEZEL_AOT_SCRATCH_DIR` at it. The link-token
directory stays where it is (it is a machine-wide flock contract,
`aot_support.hpp:494`); the bincache stays on the NVMe. `lane_setup.sh` already
has the mountpoint-guard idiom (#3008) to refuse when the mount is absent.

Cost/benefit: removes every workdir create/write/unlink from the SSD (per cold
ASan test ≈ 2.5 MB written and deleted, ~11 files; ~19k tests × 2 oracles) and
makes the 265 `temp_directory_path` sites and the 171 bypass tests uniform. What
it does *not* remove is the store's fsyncs, and it re-introduces the copy
fallback for stores and hits — cheap on tmpfs→NVMe (memcpy of 2 MB plus the
NVMe write it would have done anyway), but the "half the bytes" saving of
2026-08-21 (two keys sharing one inode) is lost unless the store links
entry-to-entry after the first copy (F5's original framing). The runaway-writer
bound changes shape: a program that prints without limit fills a 4 GB tmpfs
(ENOSPC, test fails) instead of a disk; the 30s run deadline (`:141-149`) and
the size cap together bound it, which is an acceptable and arguably better
failure than 256 KiB slurp of a multi-GB file.

Verdict: worth doing, but only after §6's A/B says it is faster on this box; on
its own it is the smaller half of the win.

### O2 — Stop paying durability for a cache that is cold 91% of the time (the "execute as built, then discard" question)

This is the lever. Three shapes, in increasing invasiveness:

1. **Gate-level: do not store on a predicted-cold gate.** `gate.cold`
   (`scripts/gate/cold.py:66-80`) already predicts cold from "core.a rebuilt" /
   entry count / identity marker. Export `BEZEL_AOT_CACHE=0` for the shard phase
   when it says cold, keep the cache on when it says warm. Zero harness code;
   the operators' remembered workaround made structural (the `gate.sh:599-603`
   lesson). What is lost: the ~95%-warm retry after a ceiling kill, and warm
   ASan on the *next* cycle if the compiler is unchanged. F6 (retryable
   classification) matters more once retries are always cold, so land F6a first.
   Also loses `[aot-cache]` telemetry on cold gates, which the trend rule R4
   reads — set the field to an explicit `disabled` rather than null.
2. **Harness-level: keep the hardlinks, drop the fsyncs.** With colocation the
   store is three links, three renames and six fsyncs; the links and renames are
   metadata ops the NVMe absorbs, the fsyncs are the only I/O that waits on the
   device. #3009 added them against a torn entry after a crash *between the byte
   copy and the rename* — a hazard that existed only on the copy path, which
   colocation removed: a hardlink has no bytes to tear, and `ld` has closed the
   file before `rename` runs. What remains is a power-loss torn write, which is
   a verify-on-read problem (record the executable's size in the entry name or
   a sidecar, refuse a mismatched hit and treat it as a miss). This reverses a
   recorded decision (#3009, pinned by
   `tests/support/aot_cache_durability_test.cpp`) and so needs an issue and a
   recorded ruling before code; the test's `storeFsyncs` assertion would flip
   to `== 0`.
3. **Harness-level: store asynchronously.** Hand the hardlinked entry to a
   background thread for fsync+rename after the test has already returned its
   verdict. Overlaps the durability with the next test's compile instead of
   removing it; more code, and the process-exit path must drain the queue.
   Declined here on CLAUDE.md's simplicity rule unless (2) is refused.

Recommendation: (1) now, (2) as an issue, (3) not.

### O3 — Run each area's tests as soon as its binary links

Feasible and the accounting already supports it (per-binary `ran == registry
total`, per-shard JSON summaries), but the payoff is bounded:

- `check` depends on the `bezel_tests` aggregate (`tests/CMakeLists.txt:285-289`)
  and `run_parallel.sh:81-86` refuses to start unless every manifest binary is
  executable. Both are orchestration, not design.
- The real blocker is the PCH: `codegen` (1,357 of 2,094 TUs) is the
  `BEZEL_TEST_PCH_OWNER` and every other group `REUSE_FROM`s it, which makes
  their *compiles* wait on the owner *target* (#3172). Moving the PCH to a
  one-TU object library that all six groups reuse decouples them (and fixes the
  `--area` compile-narrowing defect as a side effect).
- Even decoupled, codegen is ~70% of the tests and its compile+link is the
  critical path; the five small binaries can overlap it, saving at most the
  smaller of (their run time) and (codegen's compile+link), i.e. tens of
  seconds of a 91s build against a 555–1465s shard phase.

Verdict: nice-to-have after O2; do the PCH decoupling regardless for #3172.
Splitting `codegen` into two or three groups by subdirectory would make the
overlap real, at ~47 MB of extra link output per group.

### O4 — The build tree

3.6 GB per lane tree, ~2,157 TUs at RelWithDebInfo `-O2 -g` with
`-gsplit-dwarf`, six ~47 MB binaries each statically carrying `bezel_core` +
`bezel_runtime` (`tests/CMakeLists.txt:45-50`). Putting this in RAM is not
recommended on a 32 GB box that already cages builds at 12–14 GB and runs up to
five lanes; the NVMe is the right home. Two cheaper levers, both orthogonal to
the `-DNDEBUG` decision the forced build type exists for
(`CMakeLists.txt:31-49`): compile the six test executables' own TUs with
`-g0`/`-gline-tables-only` (the compiler library keeps full debug info; a
crash in the forked `Driver::compileFile` child is in `bezel_core` frames), and
consider building `bezel_core`/`bezel_runtime` as shared objects to collapse
the six-fold duplication — the latter interacts with `--export-dynamic` for the
deprecated JIT path and with the cache key hashing `libbezel_core.a`, so it is
an issue, not a PR.

### Declined

- **Bincache in RAM.** ~30 GiB per ASan pass, 64 GiB cap, on a box with ~25 GiB
  available. Also low value at a 91% cold rate; O2 makes the point moot.
- **memfd / `fexecve` surgery in the harness.** The three external tools
  (`clang`, `ld`, `execv`) each need a path, and `/proc/self/fd/N` paths would
  work, but a size-capped tmpfs gives the same RAM residency with zero code
  and no new failure modes. Revisit only if O1's A/B shows tmpfs create/unlink
  overhead is itself measurable.

## 6. Measure before changing anything (all on the dev box, none here)

1. Post-colocation cold shard phases, to replace the pre-fix 634s/1465s pair:
   ```sh
   python3 -c "import json;[print(r['ts'],r['mode'],r.get('cold'),r.get('aot'),r.get('phases',{}).get('shard')) for r in map(json.loads,open('/var/tmp/bezel-scratch/gate-timings/gate-timings.jsonl')) if r['ts']>'2026-08-21']"
   ```
2. The O2 A/B on the same 5,484-test `codegen_` slice `measured.md` used, ASan
   and plain, conc=8, empty cache: `BEZEL_AOT_CACHE=0` vs cache on. If the
   cache-off figure is still lower after colocation, O2(1) is justified on its
   own numbers.
3. The O1 A/B, same slice: `BEZEL_AOT_SCRATCH_DIR` on a fresh 4 GB tmpfs vs the
   NVMe, each with cache on and off — four runs. Record `store_fsyncs`,
   entries sharing an inode, and peak `MemoryCurrent` of the suite scope.
4. `df -i` and `df -h` of the tmpfs after each run, to confirm the reaper and
   RAII cleanup leave it empty — the failure class every recorded incident was.

Do not measure on a busy box (`measured.md §6` lost a linker measurement's sign
to a concurrent suite run).

## Status

Research note — not spec-affecting. Read-only session on 2026-09-02, HEAD
09c1803; figures are quoted or derived as marked and are specific to the dev
box (20 logical cores, 32 GB, `/` on a 7200 RPM ST2000DM008, `/mnt/fast` on a
Samsung 980 NVMe, `/tmp` a 16 GB tmpfs with a 12,774 MB per-user quota).
