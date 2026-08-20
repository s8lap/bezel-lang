# Gate performance audit (2026-08-19)

## Summary

A user-visible landing (`scripts/gate_and_push.sh`) costs **~67 min cold with
zero retries, ~78 min expected** (with retry probability priced in), and
**~29 min warm — but the warm path rarely obtains** (three separate defects
push warm-eligible gates back to cold, see F10–F12). The two sharded AOT
passes are **~75–80% of the wall**; everything else is minutes, not tens of
minutes.

Three headline results, each of which contradicts something the tree's own
comments currently say:

1. **The ASan pass's ~2x wall is a concurrency artifact, not instrumentation
   cost.** Plain: 1018.30s at conc=16. ASan: ~2016s (untruncated) at conc=8.
   Worker-seconds are nearly equal. The conc=8 cap is correctly calibrated —
   but for the **12G** `dev_heavy.sh` suite cage the run is actually in, not
   the "~24–28G slice" the comment at `gate.sh:285-291` sizes it against.
   Raising the cage (not the box) to 15–16G supports conc=10 and saves
   ~400s/cycle. This is the cheapest large lever.
2. **The gate cannot see its own dominant cost.** AOT-cache hit/miss stats
   have never been collected (the `#3012` stderr race disabled them; the
   `#3014` file sink that fixes it exists and is unit-tested but is wired
   nowhere). Consequently the single most decisive number — the per-test
   split between front-end, LLVM codegen, clang link, and harness overhead —
   has **never been measured**, and it decides which of the big levers (K0
   bitcode cache, `-fuse-ld`, harness surgery) is worth building. Measure
   before building anything large (Tier 0 below).
3. **Verified small fixes stack to ~1.4x on cold-cycle wall. 5x is only
   reachable per *validated change*** — by repairing the warm path for
   test-only batches and by making batch size an explicit policy — and the
   honest multiplier depends on the current effective batch size, which
   should be measured from `gate-timings.jsonl` before quoting any factor
   (§"What 5x actually means").

Provenance: produced by a read-only audit in a remote session (no build — an
LLVM version mismatch prevents compiling here), so every number is either
(a) a measurement already recorded in the tree's comments/telemetry docs,
(b) counted from source, or (c) an estimate marked as such. A multi-agent
pass mapped the six subsystems, built the cost model, proposed improvements
under three lenses, and adversarially verified the top nine proposals
against the code; the corrections from that verification are folded in
below (several proposals' savings shrank 2–10x; one was refuted outright).
All `file:line` citations are pinned at commit `088fb13`; line numbers
drift, so treat them as anchors, not gospel — this file already caught two
stale in-tree citations of the same kind (F16).

## Cost model of one landing

### Anchors (measured, from the tree itself)

| Anchor | Value | Source |
|---|---|---|
| plain shard phase, cold, conc=16 | 1018.30s, rc=0 | `gate.sh` #3222 block |
| ASan shard phase, cold, conc=8 | 1915.61s, killed at the 1800s ceiling at ~95% ⇒ **~2016s untruncated** | same |
| warm whole-suite plain wall | 358s | `scripts/gate/cold.py` |
| per-test wall | plain 0.12–0.26s; ASan 0.25–0.26s (ratio 1.37x, **not** 2x) | `docs/research/2026-08-10-dual-oracle-gate-investigation.md` |
| shard phase share of a cycle | 73% (plain) / 70% (ASan) | same |
| suite | ~15.3k gtests, 6 per-area binaries; codegen ≈ 12k macros (~80%) | counted |
| lit corpus | 2,174 `.bzl`, 3,233 `RUN:` lines | counted |
| conformance | 571 manifest rows, **177 runnable** / 394 skip, strictly sequential, one `systemd-run --scope` per invocation | counted; `conformance.py` |

### Cold — a compiler-source change (the common case for bug-fixing)

Any `src/` edit rebuilds `libbezel_core.a` ⇒ the KE and K1 cache keys (which
hash the archive) miss for **100%** of both oracle populations; only K2
(link key, over object *content*) can survive. Model, with the adversarial
corrections applied:

| Segment | Cold s (central) | Notes |
|---|---|---|
| cycle preflight (lock, staleness, vacuous, seam) | ~27 | |
| plain gate.sh: configure + build + cold-eval | ~140 | build = 1 core TU + `ar` + 6 test-binary relinks (~600 MB) |
| plain gate.sh: lit | ~20–35 | corrected down from 60s (est) |
| plain gate.sh: tidy | ~130–165 | est; **not** ~0 — an earlier "analyzes zero files" reading was wrong |
| **plain shard block (canary + authoritative)** | **1018 (measured)** | |
| ASan gate.sh: configure + build (no-op) + duplicated lit/tidy/cold-eval/enumerate | ~170–220 | all oracle-independent, paid twice today (F7) |
| **ASan shard block** | **~2100** | 2016 derived + fan-out overhead |
| conformance (once/cycle) | ~60–270 | est; sequential + per-case systemd-run D-Bus round trip |
| trend + push + post-push GC | ~35 | |
| **TOTAL, zero retries** | **~4,000–4,100s ≈ 67 min** | |
| retry expected value | **+~640s** | p(retry)≈0.12 plain / 0.20 ASan; a retry re-enters gate.sh at phase 1 |
| **Expected cold landing** | **~4,700s ≈ 78 min** | |

The two shard blocks are ~3,120s = **~77%** of the zero-retry total. The
**cold-vs-warm delta alone is ~1,700–1,960s per cycle** — the price of the
compiler-identity cache key, paid on every compiler change by design.

A retry is cheaper than it looks but still heavy: the failed attempt's
inline per-test cache stores leave the bincache ~95% warm, so a retry costs
~310s (plain) / ~500s (ASan) plus the 60s sleep — not a full cold re-run.
This is also why "resume across attempts" was refuted (Tier 3).

### Warm — test-only change, bincache hot

~1,715s ≈ 29 min, of which plain 358s (measured) + ASan ~716s (derived,
358 × 16/8) + the same fixed segments. A warm test is SHA-256 → cache
lookup → **byte copy (not hardlink — F4)** → exec, ~23ms/test.

**The warm path is fragile; F10–F12 each independently degrade it to cold:**
a ceiling-killed 95%-complete pass leaves the store populated but the
identity marker unwritten; a toolchain (`dnf`) update is invisible to the
gate's identity but not the harness's; and GC size over-accounting (F5)
evicts entries early. Repairing these is what makes test-only landings
actually cheap (~4.3x for that traffic class).

## Structural findings

**F1 — ASan concurrency is caged, not memory-starved.** Every `dev_heavy.sh
suite` op — plain and ASan alike — runs under `MemoryMax=12G,
MemorySwapMax=0` (`dev_heavy.sh:70-80`), not the 24–28G slice budget the
`gate.sh:285-291` comment cites. Plain fits conc=16 in that same 12G cage,
which pins the ASan *mean* at ~1.5G/shard (the 2.5–3G figure is a transient
peak) and makes conc=8 correct **for a 12G cage**. Lever: make the cage a
variable (`mem="${BEZEL_SUITE_MEM:-12G}"`), raise to 15–16G, set
`GATE_SHARDS=10` for ASan. Verified estimate: ASan shard 2016 → ~1600–1650s
(−350–450s/cycle). Do NOT raise the link-token pool with it (the pool is a
machine-wide flock contract; a gate at N=16 beside a lane at N=10 breaks
mutual exclusion, re-creating the #1780 fork/RAM exhaustion). Ship a cgroup
`memory.peak` poller first — `systemctl show <scope> -p MemoryPeak` after
the run reads a released transient unit, i.e. nothing.

**F2 — zero cache sharing between oracles, one level too late.** `sa=` sits
in both the KE and K1 key composers, yet AddressSanitizer is a post-IR
module pass (`src/codegen/LlvmBackend.cpp:87-113`): lex/parse/sema/
ownership/DA/IR-construction are bit-identical across oracles. A K0
pre-sanitizer bitcode level keyed without `sa=` would let the ASan pass
reuse the plain pass's front-end. **Corrected value: only ~75–215s/cycle**
(front-end is an estimated 5–12% of an ASan test, not 25–45%) — and it adds
compiler-code surface (bitcode serialization is new API; the static-LLVM
CMake fallback lacks `bitwriter` entirely), so per CLAUDE.md's simplicity
rule it needs an issue and, before that, the Tier-0 measurement showing the
front-end share is ≥20%. Otherwise decline.

**F3 — the gate is blind to its dominant cost.** `gate.sh` harvests
`[aot-cache] hits=` from a log the stats never reach; `BEZEL_AOT_CACHE_STATS`
is deliberately unset (#3012 stderr race); the `aot` telemetry field is
permanently null and trend rule R4 is dead code. The #3014 file sink
(`aot_support.hpp` ~:1507-1520) exists and is unit-tested. **One config line
(`BEZEL_AOT_CACHE_STATS_FILE` in `run_parallel`'s SHARD_ENV) + a merge in
the harvest phase turns the dominant cost from an inference into telemetry.**

**F4 — the hardlink fast path is dead (EXDEV) on the dev box.**
`installFromCache`/`storeToCache` assume cache and workdir share `/var/tmp`
(`aot_support.hpp:1632`), but the bincache root is a symlink to
`/mnt/fast/aot-bincache` while `scratchBase()` is hardcoded to `/var/tmp`
⇒ every hit and every store is a full byte copy. Direct wall saving is
small (verified ~10–45s cold, ~10–25s warm — the copies are already inside
the 358s warm anchor), but fix it anyway for IO sanity: cheapest form is
symlinking `aot-harness` to `/mnt/fast` too (zero code). ⚠ If taken, audit
every scanner for `-L` first — #3000 (`find`) and #3004 (`du`) are two
recorded instances of a symlinked root silently defeating a scan, and
gc_scratch's reapers key on `/var/tmp` paths.

**F5 — every executable is stored twice and GC counts it twice.** The miss
path stores the same exe under the link key and the exe key as two
independent copies; `maybeGcCache` sums per-entry sizes with no inode
dedupe. The store's apparent size is up to 2x real ⇒ **premature eviction**
— plausibly the recorded 2026-07-23 "cold-by-eviction" incident, and one of
the three warm-path killers. Fix: inode-aware size accounting in the GC
walk + credit `total -=` only when the unlink dropped the last link. (The
original "hardlink entry-to-entry" framing is secondary; the GC fix is the
value.)

**F6 — a ceiling-killed shard BAILS the cycle instead of retrying.** A
killed shard writes no JSON summary; the run exits via the
`check_agreement` short-circuit **before** #3222's `timed_out`
classification can speak; `gate_arbitrate` finds no markers and returns 2;
`gate.sh` fails; `gate_full_cycle` treats rc=1 as a **real failure and
bails** — a human then re-runs a whole cycle for what was an infra timeout
at 95% complete. Two fixes, in order: (a) run the account/classification
before the agreement short-circuit so a ceiling kill classifies as
retryable; (b) have `test_main.cpp` catch SIGTERM (the `timeout -k 10`
grace) via a **flag checked in the test loop** — not allocation in the
handler — and write the summary with `truncated:true`, plus the legacy line
(the #3012 dual-read pairs them; writing only the JSON trips the same
mismatch banner). Crediting truncated counts toward acceptance is a
separate, contract-changing step — decline for now (#3002/#3011: "a pass
over a truncated run is not a pass").

**F7 — oracle-independent phases are paid twice.** configure, cold-eval,
lit (~20–35s), tidy (~130–165s), coverage-enumerate run identically in both
gate.sh passes; `gate_full_cycle.sh:474-483` already states the
once-per-cycle argument and applies it only to conformance. Fix in place:
on the second (ASan) pass, `rec_skip` lit/tidy when `BEZEL_GATE_CYCLE=1`
— do **not** move lit to the cycle's tail (that trades ~10s for a ~3,800s
regression in time-to-red on a lit failure), and mark the skipped phases'
telemetry fields null, never 0, and guard the `COV_LITTESTS` harvest of the
stale `gate_lit.log` (#3010 class). Verified saving: ~130–260s/cycle.
**Do not overlap these phases with the shard pass**: ninja takes no
build-dir lock, conformance's 4G cages sit outside the slice budget, and
the orchestration state is exactly the #3158 trap-class. Overlap was
adversarially refuted; dedup was confirmed.

**F8 — conformance is sequential with per-case systemd-run overhead.** 177
runnable cases, each invocation a D-Bus round-trip-caged scope. A bounded
worker pool (4–6) in its **current position** is sound and verified-safe —
but the sequential caging is documented as deliberate, so it needs a
recorded decision, and the pool must keep per-case cages inside a bounded
total claim.

**F9 — `tests/module`'s ~1,366 tests bypass the AOT cache entirely.** Only
3 of 189 module files use `aot_support.hpp`; the rest hand-roll
compileFile/compileFileForTest/`std::system` (~166+ sites) — full
front-end + clang link + exec on every pass, every oracle, every retry,
uncached and without fork isolation for the compile. ~11% of the heavy
workload that every cache lever silently excludes. Port the single-module
cases to `runAot`, or give `driver_run_support` a K1/K2-keyed cache using
the existing key composers.

**F10 — the identity marker is only written on a fully-green pass** (rc==0
and COV>0), so the #3222 shape (killed at 95%) leaves a ~95%-populated
store unmarked and the next cycle predicts COLD on the ASan oracle even for
a test-only change (29 min → ~52 min). Write the marker whenever the store
was genuinely populated under the current identity, or key the prediction
on store contents rather than a green ceremony.

**F11 — the gate's identity is not toolchain-aware but the harness's is.**
`gate/identity.py` digests `core.a ‖ runtime.a`; the harness key also folds
in clang/ld/libLLVM (#3006). After a `dnf` toolchain update the gate
predicts WARM (900s ceilings) while every harness key misses ⇒ ceiling
kills. Align `identity.py` with the harness's #3006 composition.

**F12 — GC eviction pressure** (see F5) is the third warm-path killer:
~30 GiB stored per full-suite ASan pass against a 64 GiB cap, double-counted.

**F13 — dev_heavy misclassifies gate phases to background priority.** The
gate-vs-producer classifier greps argv for the literal `bezel_tests`;
configure, lit, and tidy don't contain it and run at CPUWeight=100 instead
of 10000. One-line classifier fix (match the gate's phases explicitly).

**F14 — `buildIdentity` is recomputed by every shard process.** ~200–300
processes/cycle each SHA-256 the ~100 MB `core.a` + `runtime.a` and popen
`clang/ld --version` (~0.3–0.5s each; 60–150 worker-s + ~400–600 spawns,
worst ratio on the canary's near-empty 96-process fan-out). Memoize via a
(path,mtime,size)-keyed file with the same fail-closed rules, or compute
once in `run_parallel` and export.

**F15 — the per-test link runs bare `clang` with no `-fuse-ld`**
(`src/driver/Driver.cpp:274-298`), i.e. whatever `/usr/bin/ld` is — on
stock Fedora, bfd. If `ld --version` on the dev box says bfd, a
`-fuse-ld=lld` (or mold) line in `linkCommandLine` is worth an estimated
~100–300ms on **every K2-missing link** (all of a truly-cold ASan pass) —
potentially the largest per-test win after F1. It is shipping-compiler
behavior and changes `linkRecipeSignature` (one-time full K2 cold), so:
issue-first, recorded decision, priced against measured K2 miss rates
(Tier 0). If the system ld is already mold, this whole item is moot — one
command discriminates.

**F16 — stale comments are steering tuning decisions.** At least six
load-bearing ones found: `gate.sh:285-291` (sizes conc against the wrong
cgroup), `gate.sh:629` ("ASan is roughly TWICE the workload" — actually
undetermined; the x2 ceiling multiplier was sized on it, and it happens to
be a good margin anyway), `aot_support.hpp:1632` (dead shared-/var/tmp
premise), `gate_full_cycle.sh:495-496` (conformance tally off by ~77
cases), `gate/identity.py` (pre-#3006 digest description), plus drifting
line-number citations (`gate.sh` cites `aot_support.hpp:1121-1122` for code
now at ~:1263). The repo's incident log (#3000/#3004/#3005/#3006/#3011) is
largely a history of stale comments producing wrong verdicts. Each fix
above should correct the comments it touches, and every tuning constant
(conc, cage, ceilings, tokens) should carry its measurement date and the
cgroup it was calibrated against.

## What 5x actually means — honest arithmetic

Savings add only where they hit different serial segments; they multiply
only at the per-change division. Baseline: expected cold landing ~4,685s.

| Step | Change | New expected wall |
|---|---|---|
| 1 | F1 cage+conc (ASan 2016→~1600s) | −416s |
| 2 | F7 dedup + F8 conformance pool | −250s |
| 3 | F6 + F10 + F11 + F5 (retry EV 640→~150s, variance collapse) | −490s |
| 4 | F2 K0, **only if** Tier-0 measurement shows front-end ≥20% | −~150s |
| | **Stacked cold cycle** | **~3,350s ≈ 56 min ⇒ ~1.4x** |

That is the ceiling for cold-cycle wall with verified changes: the shard
blocks contain real per-test compile work against ~10 physical cores, a
scheduling-perfect floor on the order of ~2,000s. **No verified stack
reaches 5x on cold-cycle wall; a claim that one does is double-counting.**

The 5x lives in wall-per-validated-change:

- **Batching**: cycle cost is nearly batch-size-independent (verified), so
  per-change wall = cycle/B. The canary caps B — selection must stay under
  `GATE_CANARY_MAX_PCT=50` (measured on this tree: B=5 → 38.9%, B=10 →
  56.8%, so B≈8 is the ceiling). At B=8: 3,350/8 ≈ 419s/change. Whether
  that is 1.3x or 5x depends entirely on today's *actual* B: at the
  measured recent equilibrium (B≈4–6) batching adds little; against a
  literal "a few changes" (B=2–3) baseline of 1,560–2,340s/change, the
  stack yields **3.7–5.6x**. Measure B first (records per pushed sha in
  gate-timings) before quoting a factor. Land the guardrail regardless:
  `python3 scripts/select_tests.py --base $(git merge-base HEAD
  origin/main)` before gating; land when selection <40%.
- **Warm-path repair** (F5+F10+F11): test-only batches drop toward
  ~1,100s ⇒ **~4.3x for that traffic class even at B=1**.
- A genuine 5x on *cold* cycle wall would need more physical cores or a
  second machine for one oracle (Tier 3), or cutting per-test compile work
  itself — decidable only after the Tier-0 per-test decomposition exists.

## Recommendations

### Tier 0 — measure first (small, ~zero risk; several later decisions block on this)

1. Wire `BEZEL_AOT_CACHE_STATS_FILE=<per-shard path>` into `run_parallel`'s
   SHARD_ENV (the #3014 sink exists to avoid the #3012 race); merge in the
   harvest phase; populate the null `aot` telemetry field (revives trend R4).
2. Coarse per-phase timers (frontend+codegen / link / exec / store) in
   `forkedCompileObject`/`forkedLink`, written to the same sink. This is
   THE decisive number (K0 vs mold vs harness surgery).
3. Cgroup `memory.peak` poller beside the shard phase (prereq for F1).
4. The read-only command list below, run once on the dev box.

### Tier 1 — no contract change (config + small script edits)

In rough value order: F1 (cage 15–16G + `GATE_SHARDS=10` ASan; tokens
untouched), F7 (skip lit/tidy in the second pass, telemetry nulls), F6a/F6b
(retryable ceiling-kill classification + SIGTERM truncated summary), F10 +
F11 + F5 (warm-path repair trio), F8 (conformance pool 4–6, recorded
decision), F13 (dev_heavy classifier), F14 (identity memo), F4 (colocation
symlink + `-L` audit), batch preflight guardrail, F16 (stale-comment sweep
riding each touched file).

### Tier 2 — recorded-decision items (issue first; some blocked on Tier 0)

`-fuse-ld` in `linkCommandLine` (F15; conditional on `ld --version` = bfd);
K0 bitcode cache (F2; conditional on front-end share ≥20%; needs the
static-LLVM `bitwriter` gap and a new cache-key invariant addressed);
`tests/module` port/cache (F9); reject-outcome cache (~600–700
expected-error tests re-run the front-end every pass by design — caching
the diagnostic fingerprint keyed on exactly the K1 inputs is sound by K1's
own purity argument, but it changes "diagnostics stay live" and needs the
decision recorded); slice-count decoupling in `run_parallel` (finer slices
for codegen — makespan is already at the work bound, but a ceiling kill
then costs 1/32 of codegen instead of 1/8: a variance lever).

### Tier 3 — considered and declined (recorded so it isn't re-litigated blind)

- **Resume/union across gate attempts** — REFUTED. The motivating event
  actually bails (F6), a retry is only ~2x a shard phase thanks to inline
  cache stores, and a cross-attempt union weakens the #3002/#3011 coverage
  floor and #2100's "one authoritative pass" — it could even launder canary
  logs into acceptance evidence.
- **Overlapping the two oracles (or lit/conformance) with the shard pass on
  one box** — refuted on build-dir races (ninja has no lock), slice-budget
  OOM (the exact spurious-undercount shape gate.sh:285-295 records), and
  #3158-class orchestration fragility. Two-runner dual-oracle (one machine
  per oracle) is the legitimate version — large, unverified here, worth its
  own study only after Tier 1 lands.
- **Push on plain + async ASan with auto-revert** — contract change the
  dual-oracle history (#2633, one real divergence in each direction)
  argues against; its own proposing lens recommended against it. The
  2026-08-10 note's conclusion (keep dual-oracle at current scale) stands.
- **ASan-on-subset per landing** — same contract change in smaller print;
  the 2026-08-10 note already priced Strategy B and declined it below ~30k
  tests.
- **Raising the AOT link-token pool alongside F1** — breaks the
  machine-wide flock contract unless every participant agrees on N (#1780).
- **tmpfs workdirs** — unpriced alternative to F4; CLAUDE.md's /tmp warning
  is about parking, not transient files, but nobody has measured it. Open.

## Have we enough information? (the user's question)

**Structurally yes — numerically, four measurements short**, all one
command away on the dev box:

```sh
# 1. Ground truth per phase + retry rate + cold/warm + actual batch size B
python3 -c "import json;[print(r['ts'],r['mode'],r['result'],r.get('cold'),r.get('total_s')) for r in map(json.loads,open('/var/tmp/bezel-scratch/gate-timings/gate-timings.jsonl'))]" | tail -60
scripts/gate_trend.sh
# count gate records per pushed sha => the real B the 5x claim turns on

# 2. Machine shape + live pressure during an ASan gate
nproc; lscpu | grep -E '^CPU\(s\)|Thread|Core\(s\) per socket'; grep MemAvailable /proc/meminfo
cat /proc/pressure/cpu /proc/pressure/memory   # while a gate runs
# poll .../bezel-heavy.slice/**/memory.peak during the ASan shard phase

# 3. Filesystem/cache layout, linker identity, ccache health
stat -c '%d %n' /var/tmp/bezel-scratch/aot-harness /mnt/fast/aot-bincache   # EXDEV check (F4)
readlink -f /var/tmp/bezel-scratch/aot-bincache; df -h /var/tmp /mnt/fast
find -L /var/tmp/bezel-scratch/aot-bincache -type f | wc -l; du -sh /mnt/fast/aot-bincache
ld --version | head -1                          # decides F15 outright
ccache -s | head -15; grep -E 'sloppiness|max_size' ~/.config/ccache/ccache.conf

# 4. Cheap corpus ground truth
grep -E 'Testing Time|Passed:' /var/tmp/bezel-scratch/gate_lit.log
for b in $(cut -f2 build/tests/test_binaries.txt); do BEZEL_TEST_ENUMERATE=1 "$b" | wc -l; done
python3 scripts/select_tests.py --base "$(git merge-base HEAD origin/main)"
```

Plus two tiny probe timings (not strictly read-only, run via
`scripts/dev_heavy.sh probe`): `time build/bezelc -c <30-line .bzl>` and
`time clang tiny.o -lm -lpthread <runtime.a> -lstdc++` — together they
bound the front-end vs link split that Tier 0's instrumentation will later
measure permanently.

## Threats to validity

- The 358s warm anchor predates recent suite growth; ASan-warm 716s is a
  pure 2x scaling assumption. The cold-penalty figure is a bound, not a
  number.
- "Worker-seconds = wall × shards" counts token-blocked shards as busy; the
  equal-work claim (F1's premise) and gate.sh:629's "twice the workload"
  are BOTH unproven — the 1% gap between predicted and measured ASan wall
  is consistent with either. Tier 0 discriminates.
- Cross-cycle K2 link-hit behavior for the ASan oracle was never measured
  (eviction may or may not have cleared prior-cycle ASan entries); it sizes
  every link-side lever, including F15's real-world value.
- Whether the plain pass is link-token-bound is non-discriminating from the
  existing anchors (1018.30 × 10 tokens and × 16 shards both "close").
- Several per-phase rows (lit, tidy, conformance, cold-eval) carry 2–10x
  correction bars; only the shard anchors are solid.

## Files & artifacts cited

- `scripts/gate.sh`, `scripts/gate_full_cycle.sh`, `scripts/gate_and_push.sh`,
  `tests/run_parallel.sh`, `tests/test_main.cpp`, `scripts/dev_heavy.sh`
- `tests/support/aot_support.hpp` (cache keys ~:1263/:1364; stores
  ~:1937-1952; GC ~:1712-1790; #3014 sink ~:1507-1520; EXDEV premise :1632)
- `src/driver/Driver.cpp:268-335` (link recipe + signature),
  `src/codegen/LlvmBackend.cpp:87-113` (ASan as post-IR pass)
- `scripts/gate/{cold,identity,arbitration,summary,coverage,telemetry,trend}.py`
- `docs/research/2026-08-10-dual-oracle-gate-investigation.md` (dual-oracle
  history, per-test measurements, Strategy-B pricing — this audit's
  protocol conclusions defer to it)
- `/var/tmp/bezel-scratch/gate-timings/gate-timings.jsonl` (dev box only)

## Status

Research note — not a spec-affecting proposal. Authored 2026-08-19 from a
read-only remote session at commit `088fb13`, at the repo owner's request,
for pickup by the next local session. Suggested first actions: run the
command block above, land Tier 0, then Tier 1 in the listed order; open
issues for Tier 2 items as their blocking measurements arrive. Nothing in
this note changes what a green gate proves; every contract-adjacent option
is either deferred to a recorded decision or declined above with reasons.

## Sign

Authored by a Claude Code remote session (read-only audit; multi-agent
map/estimate/propose/adversarial-verify workflow, 20 agents).
