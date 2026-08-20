# Gate performance: the audit's model, measured (2026-08-21)

Companion to `2026-08-19-gate-performance-audit.md`. That note was produced in a
read-only remote session with no build, so every figure in it was either read
off an in-tree comment or estimated, and it says so. This note is what happened
when the estimates were measured on the dev box.

**Headline: the audit's three ranked levers are worth ~3%, ~9s, and ~9% of
traffic respectively. The thing that actually costs the gate time is that the
AOT test workdir sits on a 7200 RPM spinning disk while the cache it hardlinks
into sits on NVMe — so every cache store is a cross-device byte copy, twice per
test.** On a controlled slice that one fact is 34% of the shard phase.

Nothing here changes what a green gate proves.

## Method

Every number below is either a record from
`/var/tmp/bezel-scratch/gate-timings/gate-timings.jsonl` (801 records) or a
controlled A/B run on this box: same suite, same shard concurrency, same
oracle, one variable. Where a run was contaminated by concurrent load it is
marked and excluded — one linker probe was invalidated exactly this way and is
not reported.

## 1. The cost model, measured

Medians over schema:2 records since 2026-08-15, `cold`, `result=pass`
(n=25 plain, n=20 `--full`):

| Segment | Audit estimate | Measured median |
|---|---|---|
| plain shard | 1018s | **555s** (55% of a 1007s plain cycle) |
| ASan shard | ~2100s | **1465s** (95% of a 1543s ASan cycle) |
| lit | 20–35s | **8–10s** |
| tidy | 130–165s | **0.4–0.7s** |
| configure+build | ~140s | **91s plain / 28s ASan** |
| **full cycle** | **~4000s (67 min)** | **~2550s (43 min)** |

The audit's "shard blocks are ~77% of the cycle" holds (measured 79%). Its
per-phase rows do not.

**F7 is dead.** Deduplicating lit+tidy across the two oracles saves **~9s**, not
the 130–260s claimed. Note the audit had *revised this estimate upward*,
explicitly rejecting an earlier "tidy analyzes zero files" reading as wrong.
The earlier reading was right: tidy is 0.4–0.7s.

**91% of gates are cold** (252 of 278 records since 2026-08-10). Cold is
inherent — nearly every change is a compiler-source change, which by design
misses the identity key. So the warm-path repair trio (F5/F10/F11), which the
audit priced at "~4.3x for that traffic class", serves ~9% of traffic.

## 2. F1 (ASan shard concurrency) — REFUTED

The audit's "cheapest large lever": raise the `dev_heavy suite` cage and lift
ASan from conc=8 to conc=10–16, for a claimed −416s/cycle.

Full suite, ASan, cold shape (`BEZEL_AOT_CACHE=0`), 18,288 tests, 0 failed:

| | wall | anon peak |
|---|---|---|
| conc=8 (today) | 634s | 0.95 GiB |
| conc=16 | **615s** | 1.16 GiB |

**Doubling shard concurrency bought 3%.** Two reasons the premise was wrong:

- **Memory was never the constraint.** Peak *anonymous* memory is ~1 GiB against
  a 12G cage. The audit sized concurrency against `memory.current`, which on
  this slice is ~all reclaimable page cache (measured `anon 0 / file 14.3G` at
  idle) — overstating load by more than 10x. Anon is what OOM-kills; page cache
  is not.
- **The link-token pool already binds at 10.** `aotLinkTokenCount()` =
  `min(physicalCores, MemAvailable/1.5GiB)` = `min(10, 15)` = **10** here. The
  plain pass's conc=16 is already token-bound; ASan's conc=8 has only 8→10 of
  headroom. The audit listed "whether the plain pass is link-token-bound" as an
  open threat to validity; it is.

`BEZEL_SUITE_MEM` was still added to `dev_heavy.sh` (default unchanged at 12G)
so the cage is a knob rather than a literal, and `scripts/gate/mempeak.sh` now
samples the shard phase into a `mem` telemetry field. But the cage is not the
binding constraint and raising it is not a lever.

A note on `memory.peak`: cgroup v2 resets it **per file descriptor**, so
`echo 0 > memory.peak` followed by `cat` (a new fd) still reads the global
high-water mark. A shell-side reset is a no-op; sample instead.

## 3. What actually costs the time

Same suite, same conc=8, same ASan oracle:

- cache **disabled** (compiles everything, stores nothing): **634s**
- real cold gate (compiles everything, stores everything): **1465s** (telemetry)

The cache-off run does strictly *more* compiling and strictly *less* I/O. The
~831s gap — **57% of the ASan shard phase** — is the store path.

Mechanism, in three facts:

1. `storeToCache()` and `installFromCache()` both PREFER
   `fs::create_hard_link` and fall back to a full `fs::copy_file`. A hardlink
   only works within one filesystem.
2. The workdir (`scratchBase()`) was hardcoded to
   `/var/tmp/bezel-scratch/aot-harness`; the cache
   (`/var/tmp/bezel-scratch/aot-bincache`) is a **symlink to
   `/mnt/fast/aot-bincache`**. Different devices (37 vs 66305) ⇒ **EXDEV** ⇒
   every store and every hit is a byte copy. Verified directly: `os.link`
   across them fails `EXDEV`, and succeeds once colocated.
3. `/var/tmp` is on **`sda` — a Seagate ST2000DM008, 7200 RPM spinning disk**
   (`ROTA=1`). So every test wrote a 2.31 MB ASan executable to rust, read it
   back to copy it to NVMe (**twice** — once under the exe key, once under the
   link key), and executed it from rust.

That is ~82 GB of HDD→NVMe copying per cold ASan pass. It also explains the
oracle gap that concurrency never did: ASan executables are 2.31 MB against
plain's 0.64 MB — a 3.61x byte ratio bracketing the measured 2.64x wall ratio
(1465s / 555s).

This is the audit's F4, which it identified but priced at "~10–45s cold,
~10–25s warm" — reading it as a hardlink-vs-copy nicety on the *hit* path
rather than as cross-device copying on the *store* path, onto a spinning disk.
It is also most of F5: the "stored twice" is real, but the GC's accounting of
it is correct (they genuinely were two files), so the fix is to stop making two
copies, not to change the accounting.

### Controlled A/B

5,484-test `codegen_` slice, empty cache per run, conc=8, one variable — the
`BEZEL_AOT_SCRATCH_DIR` override added with the fix reproduces the old
placement exactly, so this is a same-binary A/B:

| oracle | workdir on HDD | workdir beside cache (NVMe) | saving | cache written | entries sharing an inode |
|---|---|---|---|---|---|
| ASan | 987s | **556s** | **−43.7%** | 21G → **11G** | 0 → **8,929 / 13,523** |
| plain | 409s | **260s** | **−36.4%** | 8.0G → **4.1G** | 0 → **9,167 / 13,852** |

An earlier 1,266-test ASan slice gave −33.6% (211s → 140s), consistent.

The inode column is the mechanism proving itself: **zero** entries shared an
inode before, ~66% after. The cache halves because the exe-key and link-key
entries become two links to one inode instead of two copies — so this reclaims
disk as well as wall time.

Both oracles gain, and plain gains nearly as much as ASan (−36% vs −44%)
despite its executables being 0.64 MB against ASan's 2.31 MB. The cost is not
purely bytes: it is also ~2.5 fsync+rename pairs per test landing on a spinning
disk.

Caveat: `codegen_` is the AOT-dense binary. The whole suite includes tests that
never compile anything (`tests/frontend` has zero `runAot` call sites), so the
whole-cycle saving will be smaller than these slice figures.

### The fix

`scratchBase()` now derives from `cacheDir()`: resolve the cache path through
symlinks and place the workdir beside it. The invariant the code depends on is
then true by construction rather than by two hardcoded paths happening to
agree. When the cache dir is a plain directory this reproduces the historical
path exactly, so a box without the symlink sees no change;
`BEZEL_AOT_SCRATCH_DIR` overrides explicitly.

A symlink would have worked and needed no code, but it would have been
untracked machine-local state — the "remembered rather than structural" shape
this repo keeps writing incident reports about (#2917/#2100/#3010).

The stale comment on `installFromCache` ("the cache and workdir share
/var/tmp") is corrected — it had been asserting the very premise that broke.
This is the audit's F16 class, and it is the reason the defect survived: the
comment said the fast path was being taken, so nobody checked.

## 4. The subprocess items — real, but seconds

| | per call | per cycle |
|---|---|---|
| **F14** `buildIdentity` per shard process (SHA-256 of a 43 MB `core.a` + popen `clang`/`ld --version`) | 141 ms | ~240 processes → ~34 worker-s ≈ **2–4s wall** |
| **F8** conformance's per-case `systemd-run --scope` D-Bus round trip | 23.7 ms (vs 0.8 ms bare) | 177 cases → **~4.2s** |

Both are worth fixing because they are nearly free, not because they matter.
The audit ranked them in *worker-seconds*, which hides that they are divided
across 8–16 parallel workers.

## 5. Defects found on the way

- **#3300 (two defects, one masking the other)** — a `#` comment sat inside a `\`-continuation in `run_parallel.sh`,
  so the `gate.summary format-failure` call lost `--timed-out` and the
  REQUIRED `--logdir`. It has therefore exited 2 on a usage error for every run
  in the script's life, and the #1861 GATE INTEGRITY banner — which names the
  crashed shard and the test it died on — **has never printed**. The usage
  error is visible in every gate log. Soundness is unaffected (the run's exit
  path is independent), but the `--timed-out` plumbing added by the narrow
  #3222 work never reached `summary.py`. Fixed, with a regression test that
  also forbids the general shape anywhere in the file.

  Fixing it immediately surfaced the defect it had been hiding: `:273` read the
  ceiling-killed shard list as `"${rp_timed_out_shards[@]:-}"`, and `:-` on an
  EMPTY array yields one empty string rather than zero args — so
  `has_defect = bool(... or timed_out_shards)` was truthy on every clean run,
  and every healthy gate would have printed a GATE FAILURE banner claiming a
  shard hit the per-shard ceiling. Observed live in four runs of 260-987s all
  reporting a phantom kill at a 3600s ceiling. Both bugs came from a4c0bdc,
  which removed that `:-` at the argument site, left it at the declaration
  site, and amputated the call with the comment explaining why. The second bug
  is the same shape a4c0bdc existed to fix, which is why its own comment
  describes it exactly.
- **F3 wired** — the `#3014` cache-stats file sink existed and was unit-tested
  but was connected to nothing, so the `aot` telemetry field had always been
  null and trend rule R4 was dead. Per-shard sinks now merge into one line that
  `gate.sh`'s *existing* harvest reads, so no gate.sh change was needed. First
  measurement ever recorded: `hits=238 misses=3 (98% hit)` on a warm
  ownership slice.
- **`/tmp` is still load-bearing.** `run_parallel.sh:88` puts the shard logdir,
  and each shard's `TMPDIR`, under `${TMPDIR:-/tmp}` — a 16G tmpfs.
  `dev_heavy.sh` documents this exact shape as having already killed a gate
  ("per-shard AOT caches rooted under `${TMPDIR:-/tmp}` exhausted the 16G
  tmpfs"). The AOT cache was moved out; the shard logdir was not. One
  consequence is an inversion: the ~171 `tests/module` files that bypass the
  AOT harness write their executables to RAM, while `runAot` tests wrote to a
  spinning disk. Flagged, not changed — moving them to disk would make them
  slower, and it deserves its own decision.

## 6. Still open

- **F15 (linker).** The system `ld` is GNU bfd 2.45.1; mold and lld are both
  installed, and `Driver::linkCommandLine` runs a bare `clang`. A first probe
  was contaminated by a concurrent suite run — its two runs disagreed on the
  SIGN of the effect — and is discarded. Needs re-measuring on a quiet box.
  Note it is Tier 2 regardless: it changes `linkRecipeSignature`, i.e. a
  one-time full K2 cold, and wants a recorded decision.
- **Whole-cycle payoff.** Both oracles are measured on an AOT-dense slice
  (above); the whole-suite figure will be lower and is best read off
  gate-timings once a few cycles have run on the fix.
- **The double store itself.** Once colocated the bytes are free, but
  `store_fsyncs=3348` for 1,266 tests is still ~2.6 fsync+rename pairs per test.
- **F9** (`tests/module` bypassing the harness) is untouched and, given the
  91% cold rate, is not a caching win — it is a fork-isolation gap.

## Status

Research note — not spec-affecting. Measurements taken 2026-08-21 on the dev
box (20 logical / 10 physical cores, 25 GiB available, `/` on a 7200 RPM
ST2000DM008, `/mnt/fast` on a Samsung 980 NVMe). Figures are specific to this
hardware; the *shape* of the finding — that a hardlink fast path silently
became a cross-device copy — is not.
