# Coordinator runbook — the long form of CLAUDE.md's operational rules

*Split out of `CLAUDE.md` on 2026-09-26 (R21), at the owner's request, to cut the
per-agent context cost. `CLAUDE.md` is auto-loaded into every agent, and was
~61 KB, most of it the evidence behind each rule rather than the rule itself.
The rules stay in `CLAUDE.md` as short bullets. This file keeps the evidence and
the procedures, and was moved here **verbatim**.*

**Authority:** if this file and `CLAUDE.md` disagree, `CLAUDE.md` wins, and the
disagreement is a defect to fix in whichever file is stale.

**Who reads this:** coordinators (orchestrators) before running a gate, a
dual-coordinator run, or a pool. Lanes read `CLAUDE.md` and
`docs/lane-contract.md`. They do not need this file unless a rule's reason
matters to their task.

**Pre-gate in one command:** `scripts/pregate.sh [--filters "…"]` runs every
build-free phase the gate runs, plus the machine preflight, and prints READY or
FAIL. The manual steps below remain the reference for what it checks.

## Build & test speed (the build is dominated by the test suite)

(This file states sizes qualitatively on purpose — exact counts like the test
total or TU count drift every run, so they are not hardcoded here. For a live
number run the suite or `find tests -name '*.cpp' | wc -l`; for a spec revision
read the relevant document's header.)

The configure auto-detects accelerators (CMakeLists.txt): **ccache**, the
**mold** linker (falls back to lld), **`-gsplit-dwarf`** (debug info out of the
link), the **shared `libLLVM`** (linked as one `.so`, not static components —
keeps binaries small and links near-instant; the in-process JIT is unaffected),
and a **PCH** of the heavy LLVM/std headers for the test target. Build with
plenty of parallelism — the machine is many-core and each TU peaks well under
the build cage's memory budget: pass `-j16` (or use `cmake --build --preset
default`, which sets jobs); the bundled scripts default to `-j16`. A clean build
is ~2 min and a one-file edit→relink is ~1–2s. ⚠ Those two figures were
measured at ~770 TUs; the tree is now **~1,760** (`python3 -c "import json;
print(len(json.load(open('build/compile_commands.json'))))"` — 1,756 on
2026-08-13, and this number drifts every run, so re-measure rather than trust it),
so treat them as lower bounds. **Do not re-measure by hand** — every gate has
appended one JSON record per run to `/var/tmp/bezel-scratch/gate-timings/`
since 2026-07-14, with per-phase timings. Read it with `scripts/gate_trend.sh`
(which also fails a push when a gate's telemetry contradicts its verdict, #3010).

**The suite is several per-area binaries, not one.** `tests/CMakeLists.txt`
splits the test sources along the existing `tests/<area>/` directories into
`build/tests/bezel_tests_{codegen,ownership,sema,frontend,module,support}`, so a
test-only edit relinks only its own area and a lane can build+run just its area
(`scripts/agent_check.sh --area sema <filter>`). **`bezel_tests` is still the
target name** (`cmake --build build --target bezel_tests` builds them all), so
build commands are unchanged; anything that needs a *path* reads the
CMake-generated manifest `build/tests/test_binaries.txt`
(`<group> <TAB> <path> <TAB> <#sources>`) — never hardcode a binary path, and
never enumerate the areas by hand. Two configure-time FATAL_ERROR invariants
guarantee the split is a true partition (every discovered `tests/**/*.cpp` is in
exactly one binary; a `tests/<newdir>/` no group claims is a hard error), so the
union of the binaries is exactly the whole suite.

**Run the test suite in parallel**, not as a raw binary — a single binary is
serial gtest (several minutes). Use `tests/run_parallel.sh 16` (or `cmake --build
build --target check`): it slices EVERY test binary across cores (each shard a
separate process via `BEZEL_SHARD_*`) and feeds the slices through one queue
whose width is that first argument — so the argument is the *concurrency* cap
(the resource knob the ASan gate lowers to 8), not a per-binary shard count.
Accounting is per binary: each one's `ran` must equal its own registry total and
every one of its shards must have printed a completion summary, so a dropped or
crashed slice in any binary still fails loudly. Tests
drive the *real* `bezelc` AOT path (fork-isolated `Driver::compileFile` + clang
link + native exec) — the strict oracle. The sharded run (`scripts/gate.sh`)
is **authoritative over the retired serial pass** (#1861): every test is
process-isolated (a fork'd compile + a separate native exec), so the old
serial-order heap-corruption class is structurally impossible and the always-on
serial pass was retired. ⚠ That authority is **sharded-vs-serial, NOT
`--full`-vs-plain** — an earlier wording attached it to `--full` and so read as
"the ASan oracle is the authoritative one", which is precisely what #2633
refuted and what the ⚠ block below says. Neither oracle dominates; a batch needs
BOTH. The sharded
runner *can* over-report under fork/link contention — but that shows up as a
whole-suite timeout, not a real failure; to disambiguate, re-run the specific
suspect test(s) in isolation with `BEZEL_TEST_FILTER=<name>` (a single-process
run), NOT a full serial pass. (The binaries silently ignore `--gtest_filter`;
`BEZEL_TEST_FILTER` is the substring selector. `BEZEL_TEST_ENUMERATE=1` lists
what a process *would* run, one `suite.name` per line, without running it — that
is how tooling finds which area binary holds a filter, and how the partition is
proved.) See `tests/README.md` for the
harness (`runAot` / `runAotChecked` / lit+FileCheck) and how to write a test;
`BEZEL_AOT_LINK_TOKENS` caps concurrent compiles when several agents run the
suite at once. ⚠ **The in-process JIT harness is DEPRECATED, not retired.** An
earlier revision of this file said "has been retired" while stating 45 lines
above that "the in-process JIT is unaffected" — both cannot be true, and the
code settles it: **60 `runJit(` + 3 `runJitChecked(` live call sites remain**
(measured 2026-08-08, re-verified 2026-08-13;
`grep -rn 'runJit(' tests/ --include=*.cpp | grep -v '^tests/support/' | wc -l`
— the `--include`/`grep -v` matter: the bare `grep -rn 'runJit(' tests/` an earlier
revision gave here returns **62**, because it also counts the harness's own
definitions under `tests/support/`. A recipe that does not reproduce the number
beside it reads as a violated ratchet; it cost one false alarm already, so if you
find these disagreeing, suspect the recipe before raising the count.)
It is a *lenient* oracle —
it skips the ownership/definite-assignment passes and `verifyModule`, so a test
still on it proves less than its name suggests. **Write new tests on `runAot`
only**, and port a JIT test whenever you touch one. The count above is the
ratchet: it must only ever go down.

**That ratchet is now executed, not just written down** (#3439).
`scripts/check_deprecated_jit_oracle.py` measures all three numbers against
`scripts/deprecated_jit_oracle.baseline` and `gate_full_cycle.sh` runs it —
self-test first, then `--strict` — alongside the seam, vacuous-test and
enumerable-grid ratchets. After **porting** a test, bank it with `--update`
(which refuses to raise a count); `--list` shows every file still on the
lenient oracle. The baseline is **exact, not a ceiling**: a count *below* it
fails as stale headroom, because unbanked headroom is indistinguishable from a
rise somebody already spent. ⚠ The reason this phase exists is worth keeping:
the rule above, with its measured numbers, had been in this file for months and
the count still rose 60→61 on 2026-08-23 and survived five days and two full
gate cycles. **A prose ratchet is not a ratchet** — its three sibling checks
were scripts with self-tests and this one was a paragraph.

One machine-local, not-tracked step is required for ccache to cache the PCH'd
compiles: `~/.config/ccache/ccache.conf` must contain
`sloppiness = pch_defines,time_macros` (without it every PCH build is a cache
miss — slower, never incorrect).

**Concurrency discipline (32 GiB box, parallel dev agents).** Run every full
build and every full suite through `scripts/dev_heavy.sh {build|suite} <cmd>`:
it runs them in a shared resource-capped cgroup slice (`bezel-heavy.slice`,
~18 cores, 24G soft / 28G hard) that bounds TOTAL cpu+mem across all concurrent
heavy ops WITHOUT serializing them — N builds share the budget (each slower)
rather than oversubscribing or OOMing. (An earlier exclusive flock was removed
deliberately: it caused a producer-stall storm; the slice bounds resources
without a lock — do not reintroduce one.) Gate ops get high CPU/IO weight so
they stay fast; producer builds run at background weight on spare capacity; each
op is also memory-caged (12–14G). Small probe compiles/runs use
`dev_heavy.sh probe <cmd>` (own small cage, no slice accounting). Scratch
artifacts (probe binaries, big logs, scratch build trees) go in
`/var/tmp/bezel-scratch` — never `/tmp`, which is tmpfs (RAM): a scratch build
tree parked there permanently consumes gigabytes. Keep ccache `max_size` at
80G (a full 40G cache thrashes under multi-worktree churn).

**The slice bounds cpu+mem, not the NUMBER of compiles** — N lanes × `-j16`
put 100+ clang on 18 cores in R20 (134 measured). The optional fix is the
machine-wide build jobserver: `scripts/jobserver_ctl.sh start` (a systemd
--user service, default 20 tokens; `status`, `stop`). While it runs, every
`dev_heavy.sh build cmake|make|gmake|ninja …` joins one shared GNU-make token
pool (its `-j` is stripped); two exceptions do NOT join: `cmake --build
--preset …` (cmake adds the preset's `jobs` itself) and a caller whose
`MAKEFLAGS` is already set. Each build keeps one implicit job, so none starves and a
one-TU relink never queues behind a cold build. With no daemon, `dev_heavy.sh`
joins nothing and the compiles fall back to the launcher's slots (below);
`gate.sh` always opts out (`BEZEL_JOBSERVER=off`). ⚠ It is
a **memory safety valve, not a speed-up** (measured R20: capped builds kept
MemAvailable ≥11G where uncapped fell to 4.9G, but took ~40% longer each).
Lanes must build through `dev_heavy.sh`/`agent_check.sh` and must NOT add their
own "wait until fewer than K builds" gate on top: a hand-rolled gate that other
lanes ignore starved one lane for 70 min in R20.
Two layers enforce this since 2026-09-30, when a coordinator's lane briefs said
`cmake --build build --target bezel_tests_sema -j` (unlimited `make -j`, outside
`dev_heavy.sh`): 499 clang, and the OOM killer took three Claude sessions and
plasmashell. (1) Every generated compile and link runs through
`scripts/build_token.sh`, which takes a pool token unless its make/ninja already
joined the pool, or one of 16 machine-wide flock slots when no launcher-aware
daemon runs. A bare `make -j` therefore queues instead of fanning out, however it
was started. (2) A Claude Code PreToolUse hook (`scripts/claude_hooks/build_guard.py`,
installed as `~/.claude/hooks/bezel_build_guard.py` in `~/.claude/settings.json`)
refuses a bare `cmake --build`/make/ninja in a build dir, a `BEZEL_JOBSERVER=off`
or `MAKEFLAGS=` override, and an Agent prompt or `*brief*` file that tells a lane
to run one, and names the command to run instead. Denials are logged to
`/var/tmp/bezel-scratch/build-guard/denials.log`. After changing the hook, re-copy it.
`start` refuses to run from a linked worktree. A *dedicated* daemon worktree (e.g.
`/mnt/fast/wt-jobserver21`) needs `BEZEL_JOBSERVER_ALLOW_WORKTREE=1`. The unit name is
`bezel-jobserver` for the default token dir, and `bezel-jobserver-<hash8>` for any other
`BEZEL_JOBSERVER_DIR` (override with `BEZEL_JOBSERVER_UNIT`, #4990).

**Producer/fix lanes: build+test your change with `scripts/agent_check.sh`.** It
is the single, opinionated entry-point that encodes the correct lane protocol so
you cannot get it wrong — it reconfigures (so a *new* test `.cpp` is picked up;
`cmake --build` alone can't see it), builds via `dev_heavy` (slice-capped,
foreground — no background waiter to stall on), removes the stale binary first,
and runs ONLY your targeted filter:
`scripts/agent_check.sh <filter> [filter2 …]` (substring on gtest suite-or-name,
NOT the file basename). You do **not** need to know which area binary holds your
test — it enumerates the binaries (~30 ms each) and runs exactly the ones that
match, so a filter spanning two areas runs in both. `--area <group>` (e.g.
`--area sema`) narrows the LINK to one ~50 MB binary instead of all ~600 MB.
**Since fix #3172 (2026-09-03) `--area` narrows the COMPILE as well as the
link.** The 12-header PCH is owned by a one-TU object library
(`bezel_tests_pch`, generated into the build dir) that all six groups
`REUSE_FROM`, so no group's compile depends on another group's target any more:
`cmake --build build --target bezel_tests_module -- -n` after touching a codegen
TU schedules **0** codegen compile actions (it was ~2,228 when codegen owned the
PCH, measured 2026-08-13). Test TUs are compiled with `-gline-tables-only` (the
compiler library keeps full `-g`), which roughly halved the six binaries
(codegen 366 MB → 212 MB). ⚠ An earlier revision of this paragraph documented
the opposite — that only `--area codegen` was narrow — and it was true until
#3172 landed; if `--area <group>` ever schedules codegen actions again, the
owner target has regressed. Budget one full test rebuild for the first gate
after any change to the test compile flags (the PCH and ccache keys both move). For a single `.bzl`:
`scripts/agent_check.sh --probe
FILE.bzl` (or `--probe --asan FILE.bzl` for memory/disposal fixes, with a hang
timeout); all probe output goes to the scratch dir, never the repo root. It
REFUSES an empty filter and `--gtest_filter` (the binary silently ignores that
flag and runs everything). **A lane must NEVER run the full suite / `run_parallel`
/ `check` — that is the integration GATE's job** (`scripts/gate.sh`, coordinator
only); a lane running the whole suite just burns the shared budget on ~100
redundant fork+link+exec tasks.

**Keep the repo root clean.** Ad-hoc `.bzl` probes, one-off `bezelc -o` outputs,
and investigation scratch do NOT belong loose in the repo root — put them in
`./scratch/` (gitignored) or `/var/tmp/bezel-scratch/`. ⚠ **Scratch goes in a
LANE-UNIQUE subdir** (`/var/tmp/bezel-scratch/<lane-name>/`), never straight
into `/var/tmp/bezel-scratch/` and never a shared short prefix. Measured
2026-09-22: two lanes both wrote a probe `.bzl` under a common directory, one
overwrote the other's file mid-run, and the victim's probe briefly read as
"the bug fixed itself" — a false refutation, which is the expensive direction
for this class of mistake. Cleanup is automated, so
you should rarely sweep by hand: `scripts/gate_and_push.sh` runs
`scripts/gc_scratch.sh` after every successful push, which (a) reaps worktrees
whose commits are already in `origin/main` (branch refs are kept; WIP, locked,
and the current worktree are never touched), (b) drops stale
`/var/tmp/bezel-scratch/build-asan-*` trees, and (c) quarantines >48h-old root
scratch into `/var/tmp/bezel-scratch/root-quarantine/` (moved, not deleted;
self-purged after 14 days). Run `scripts/gc_scratch.sh` (dry-run) any time to
preview, `--apply` to act; `scripts/reap_orphans.sh` clears processes a dead
agent orphaned. ⚠ **Per-worktree build dirs do NOT necessarily live inside the
worktree.** `scripts/lane_setup.sh` symlinks a lane's `build/` out to
`/mnt/fast/lane-builds/<name>` (the NVMe), so `git worktree remove` deletes only
the symlink and **orphans the tree**. That is not theoretical: 68 GB accumulated
across 54 orphans in four days and had to be cleared by hand (#3005), while this
file asserted the invariant that says it cannot happen. `gc_scratch.sh` **now
does** reap them (an earlier revision of this line said it did not — verified
2026-08-13, it reaped three 3.6 GB trees), but it only reaps trees with no
matching worktree, so a wave of N live lanes still holds N trees at ~3.6 GB
each. Watch `df -h /mnt/fast` during a fan-out: five concurrent lanes took it to
**91%**, and a lane that runs out of disk mid-build fails in ways that look like
a compiler error. Re-run `lane_setup.sh` in a worktree whose `build/` symlink
dangles (`scripts/lane_setup.sh` self-repairs dangling build/ symlinks).

⚠ **AOT bincache + workdir must be on a fast disk** (the original `#3010`-class
defect — silently caused a 5.7s/test cold-cache wall time on 2026-08-22 Wave
4 attempt-1, exceeding the 1800s shard ceiling per-test). The AOT harness's
`cacheDir()` and `scratchBase()` defaults (`tests/support/aot_support.hpp:1014`
and `:153`) both resolve to `/var/tmp/bezel-scratch/{aot-bincache,aot-harness}`
— `sda3 btrfs` on the machine this was measured on (~5 durable creates/s,
~65ms fdatasync on a 1 MB file). The fast disk on that box was `/mnt/fast`
NVMe (~846/s, ~11ms fdatasync — a 6× per-op gap, 169× on sustained creates).
Other machines will have other "slow" and "fast" disks; the policy is
**structural, not FS-type-specific**.

**Setup is the operator's responsibility**, not something `gate.sh` probes for.
`gate.sh` deliberately does not measure fsync speed or check filesystem type:
thresholds are machine-dependent (a busy NVMe under contention is ~5ms,
indistinguishable from sda3 btrfs quiet — false positives), and an unfamiliar
slow backend (xfs-on-SATA, USB, network mount) would slip past an FS-name
check. The cold gate's *behavior* — per-test shard phases hitting the 1800s
ceiling with `runs/OK` counts in the 300s — is the load-bearing indicator.

**The fix is `scripts/lane_setup.sh`** (run as the first command in any lane
worktree; idempotent; refuses to clobber a real dir). It symlinks both
`/var/tmp/bezel-scratch/aot-bincache` and `/var/tmp/bezel-scratch/aot-harness`
to `/mnt/fast/{aot-bincache,aot-harness}` when `/mnt/fast` is a mount point.
Override the path with `BEZEL_AOT_FAST_ROOT=/your/fast/disk` if the fast disk
isn't named `/mnt/fast`. To opt out of symlinking entirely (e.g. you have a
single-disk box or want to use a tmpfs), set `BEZEL_AOT_FAST_ROOT=""` or set
`BEZEL_AOT_CACHE_DIR` / `BEZEL_AOT_SCRATCH_DIR` env vars to point at your
chosen locations — both the harness defaults and `lane_setup.sh` respect them.
dangles (it now self-repairs).

**Waiting for a gate (this is where coordinator time actually goes).** Two rules,
both learned expensively:

1. **Never wait on a process pattern.** `until ! pgrep -f '<pat>'` self-matches,
   because the waiter's own command line contains the pattern. Bracketing
   (`[p]at`) is NOT a general fix — it only defeats the self-match when the
   pattern appears solely as grep's argument. Two such loops cost ~3.2 h in the
   2026-08-12 run, one of them spinning for hours over a gate that had passed.
2. **Run the gate DETACHED, and wait on identity.** A gate cycle is ~50 min; a
   harness-tracked background job can be killed at its own timeout, which
   silently leaves the orphaned shards running and writing to the log — it looks
   exactly like a gate still in progress. Launch with
   `setsid nohup scripts/gate_and_push.sh > LOG 2>&1 < /dev/null &`, then wait
   with `scripts/wait_pid.sh "$BEZEL_GATE_PIDFILE"` (identity, via `kill -0` —
   it cannot match itself).

`gate_and_push.sh` publishes `$BEZEL_GATE_PIDFILE` (removed at exit) and
`$BEZEL_GATE_DONEFILE` (the exit status) — but only once it is **committed to
gating** (#3463): a `--help`, a refusal, or the "another gate is running" abort
publishes nothing, because those files belong to whichever gate is actually
running. Before that fix every invocation wrote them, so the #3415 self-test
phase deleted the live gate's pidfile and wrote `1` into its donefile 20s into a
cycle that went on to pass, and `wait_pid.sh` exited ~50 min early. Publication
therefore lands ~50ms in rather than immediately, so `wait_pid.sh` now waits a
bounded moment (`--pidfile-grace`, default 30s) for the pidfile to appear
instead of failing the launch race. ⚠ Prefer the PIDFILE: the donefile is
written from an EXIT trap, so an uncatchable kill produces no marker at all, and
a donefile-only waiter then hangs forever over a finished gate. That is not
hypothetical — the marker was **dead on arrival** from the day it was added
until #3158, because a second `trap … EXIT` later in the same script silently
replaced the handler (bash keeps exactly one). `scripts/gate/tests/test_gate_and_push_trap.py`
pins that a second one has not reappeared, and **since #3415 it is gate-enforced**:
`gate_full_cycle.sh` runs `python3 -m unittest discover -s scripts/gate/tests`
as the `gate-selftest` phase, beside the vacuous-assertion and imported-symbol
ratchets — build-free, ~33s, fail-closed. The same now holds for
`test_telemetry`, `test_canary`, `test_gate_serialization` and
`test_repo_identity`. ⚠ It is a **gate** phase, not a *build* gate: `cmake --build`
still does not run it, so run it by hand while iterating on gate infrastructure.
Two earlier revisions of this passage were wrong in opposite directions — one
claimed it "now fails the build" when nothing invoked the directory at all, the
other still said "MANUAL check" after it was wired; check `gate_full_cycle.sh`
for the phase before trusting either.

The phase ratchets **counts, not just the exit status**, because `unittest` exits
0 for "Ran 0 tests" and for "OK (skipped=590)" alike: it asserts a floor on tests
run and a cap on tests skipped, so a collection failure or a muted directory
fails the cycle instead of passing with a green tick. Two tests in
`test_integration_bash_python` drive the real `gate_and_push.sh` against the real
lockfile and **self-skip with a printed reason** when a gate is running or the
tree is dirty — they would otherwise `unlink` a lockfile a live gate holds
(letting a second gate start on a fresh inode), and the entry point refuses on a
dirty tree by design (exit 65). Inside a gate both hold, so **2 skips is the
expected steady state**. (An earlier revision attributed the lockfile hazard to
`test_gate_serialization`; that file isolates every lock into a
`TemporaryDirectory` and never touches the production path.)

**The tree is FROZEN while a gate runs.** Not just `src/` — a mid-cycle rebuild
makes a mixed-HEAD gate — but `scripts/` too: bash reads a script lazily by byte
offset, so editing one mid-run desynchronises the running shell, and restoring
the original bytes afterwards does NOT save the run. Park edits in
`/var/tmp/bezel-scratch/<run>/parked/` and re-apply after the gate reports.

**Gating a batch: BOTH oracles, always.** Land with `scripts/gate_and_push.sh`
— it runs `scripts/gate_full_cycle.sh` (plain, then `--full`), requires each to
be trustworthy-green, and only then pushes. `--full` forces generated-code
AddressSanitizer on every test program (`BEZEL_AOT_FORCE_SANITIZE=address`), so
the whole suite runs under ASan in that one sharded pass — the former standalone
`asan_sweep.sh` is folded in (#1861), and no serial pass is needed because AOT
process-isolation makes serial-order corruption structurally impossible.

⚠ **`--full` is a DIFFERENT oracle, not a stronger one (#2633) — neither
dominates, so one is never sufficient.** ASan replaces the allocator and changes
layout, so a wild read that FAULTS on the plain build can land in mapped memory
and pass under `--full`; conversely a silent overflow the plain build tolerates
is only caught by ASan. #2633's evidence is one binary, one filter: 3 failures
plain, 0 under ASan, where the bug was a SIGSEGV on the default build. Because
runs had been gating almost exclusively with `--full`, that whole defect class
was "structurally invisible to the gate, however many times it ran".

`scripts/gate_and_push.sh --full` still works and is now a **no-op** (the cycle
always runs ASan). `--quick` is REFUSED for a push. A deliberate one-oracle gate
is `--single-oracle MODE`, which says loudly that it is not a full acceptance.
Running `scripts/gate.sh` directly gates ONE oracle and now prints that its
`GATE PASS` is not an acceptance verdict — use it for diagnosis, not to land.

## Dual-coordinator runs (two sessions, one repo)

A run may be driven by **two coordinator sessions** sharing one repo, one machine
and one `origin/main`, each with its own pool. That configuration has run for
several cycles and is productive — the serial bottleneck is coordinator
cognition, and two of them genuinely double it. Every rule below was paid for in
a lost gate cycle or a corrected memory file during those runs.

⚠ **None of this was written down until 2026-09-24.** The protocol lived entirely
in one coordinator's private memory directory, which lane agents never read and
which the two coordinators had already collided on. If you are the second session
and this section is all you have, it is enough to start.

### One gate owner, declared, with an explicit verdict

The coordinator that launches the gate **owns the freeze**. The other confirms
**zero agents and zero builders by artifact** (`scripts/no_builders.sh`, never a
`pgrep` recipe) and holds until it receives an explicit `PASS` / `FAIL` /
`BANKED` verdict. **Silence is not a verdict** and a green-looking log is not
one either. Send the stop-order and the verdict as messages; do not assume the
other session is watching your log.

### A peer message is the WEAKEST evidence tier

Ranked, strongest first: **the spec** → a **primary artifact** (a gate log, a
diff, a test row's own output) → **a query you ran yourself** over those
artifacts → **a lane handback** → **a peer message**. A peer message has passed
through one more summarisation step than a handback has, and it arrives in
conversational prose that reads more confident than the evidence behind it.

⚠ Measured 2026-09-24: one coordinator sent the other a *prime suspect* for a
9-row gate failure. The receiving session banked it into memory as mechanism
within minutes. The lane then proved it right for 4 of 7 rows and **wrong for
the 3 that were actual soundness regressions** — the opposite fix (narrow the
compiler) from the one the banked note implied (flip the tests). The peer
boundary strips provenance: what one side writes as a hypothesis arrives on the
other side as a bullet point.

**So label every claim you send with its tier — `MEASURED:`, `QUOTED:`,
`HYPOTHESIS:` — and never upgrade a tier on receipt.** When a peer sends an
unlabelled claim, treat it as `HYPOTHESIS:` until you have re-derived it.

### The memory directory is shared, mutable state

Both coordinators write to the same `memory/` directory. **Read a file before
replacing it.** Index lines in `MEMORY.md` are append-safe (both sessions insert
lines and they merge cleanly); *content files are not*. Measured 2026-09-24: one
session overwrote a file the other had written minutes earlier, and the lost
point was recovered only because the surviving index line happened to quote it.

### ⚠ Move a rule to whoever has a reason to run it

The most expensive dual-pool defect of cycle 24 was **not a missing rule**.
CLAUDE.md already said *"Before committing, `git grep` every test named in the
commit message and every test in the files touched."* Two lanes skipped it, both
§17 reviews passed anyway, and the gate failed on 9 rows across 2 commits — one
of them a genuine soundness regression. Both commits were authored in one pool
and integrated by the other.

**An author has no incentive to hunt for tests that contradict them; an
integrator does.** So when pool A authors and pool B integrates:

- **The integrating coordinator runs the behaviour-flip grep ITSELF before
  cherry-picking**, for any commit whose body claims to admit what was rejected,
  reject what was admitted, rename a diagnostic, or change an allowlist. It is
  one `git grep` and needs no context — which is exactly why it survives being
  moved across the pool boundary.
- The author's §17 review still happens in the authoring pool. It is not a
  substitute: a reviewer briefed by the same coordinator shares the author's
  framing.

This generalises past the peer case: **a check that only its author can run will
be skipped, however prominently it is written down.** Prefer the enforcement
point with an independent motive.

### A diagnostic-text rename is a behaviour-flip

Grep every needle, in **gtest and lit both**. A lit `CHECK-NOT` is the dangerous
half: when the text it names stops occurring, the check passes **vacuously** and
goes green rather than red.

⚠ Measured 2026-09-24, and it stacked across two commits an hour apart in one
batch: `#4735` renamed `§28.3 auto-drop` → `§28.3 must-use / auto-drop`. That
broke one gtest needle — **invisibly**, because a sibling commit had already
suppressed the warning at that site, so the row was red for the other reason and
the stale needle never earned its own failure. Two defects, one row, both had to
be fixed. It also left 10 `tests/lit/sema/*.bzl` `CHECK-NOT`s passing
vacuously.

### Keep a batch ledger naming the authoring pool

The integrating coordinator records, per commit in the batch, **which pool
authored it**. The commit trailer cannot carry this (exactly one mandated
trailer, no second line), so keep it in the run's scratch dir. On a gate FAIL
this routes each cluster to the coordinator holding its context, instead of the
integrator re-deriving from scratch — cycle 24's FAIL was 9 rows from 2 commits,
both from the other pool.

### Batch-level banks belong to the integrator

`check_toolchain_skip_surface` is banked **once per batch, by the integrating
coordinator** — not per lane and not by both sessions. Two pools adding
`runAotChecked` rows otherwise collide on the single baseline file.

### Peer heartbeats report events, not ticks

*Added 2026-09-28 (R23), at the owner's request.* A peer's cron is its own
wake-up backstop; it is **not** a reporting schedule. With two peers each
sending a full roster on every tick, the integrating coordinator spent most of
its turns re-reading status that had not moved, and the few actionable lines
(a handback SHA, a slot freed) were buried in tables.

A peer messages the integrator **only** when something is actionable or has
changed:

- a handback or review verdict is ready (with its SHA);
- a blocker, or a question that needs a ruling;
- a cold-codegen slot freed or taken;
- a resource alarm (`MemAvailable` < 8G, sustained swap-out, `/mnt/fast` > 85%);
- an acknowledgement of a drain or gate order.

On a no-change tick it sends **nothing**. At most once every two hours it sends
one line — `<peer> alive: N lanes, no change`. Batch several events into one
message; no roster tables or restated protocols unless the integrator asks.

The peer **keeps** its ~30-minute cron — only the message is suppressed, not
the check. (One peer read "fewer reports" as "sparser cron" and started
removing its own backstop.) Correspondingly, the integrator no longer treats
silence as an alarm: it pings a peer only after ~2 h of silence or when an
event it is waiting on is overdue.

## Coordinator tooling

*Added 2026-09-26 (R21). In R21 most coordinator time that was not review went
into manual checks, and each one was skipped at least once. These scripts make
them mechanical. Each script's header has the details and the incident behind it.*

| When | Tool | What it replaces |
|---|---|---|
| Every heartbeat | `scripts/run_status.sh [--staging <wt>] [--target N]` | eight hand-typed probes: gate pidfile/donefile/cycle log, jobserver unit, builders, MemAvailable + swap-out, `/mnt/fast` use and the aot symlinks, staging count vs target, lane count. Prints `ACTION:` lines only for flagged rows; never acts. |
| A lane hands back | `scripts/handback_check.sh <lane-branch> [--base REF] [--reported-sha S] [--grant G,…] [--author 'N <e>']` | the manual handback checklist: reported SHA is the tip (else: in the branch, elsewhere, or dangling), base is an ancestor, trailers, `git config` author leak, files outside the grant, `/home` paths in new scripts and hooks, stale needles, tests named in commit messages exist. Read-only (pinned by a test); FAIL exits 1, WARNs are for the reviewer. It does not replace the non-author review. **Mid-batch, measure against staging:** `--base <staging-branch>`, or export `BEZEL_STAGING_REF=<staging-branch>` once (the default when set; else `origin/main`). |
| Picking onto staging | `scripts/pick_stage.sh --branch … --pool … --lane … --reviewer … --verdict ACCEPT\|ACCEPT-W-NOTES --kind fix\|test\|bank\|infra SHA…` | the scratch pick script and the hand-kept batch ledger. Records pool, lane, orig SHA, reviewer, verdict, kind and issues (the `#NNN` in the subject only) as a note under `refs/notes/bezel-picks`. Refuses a dirty tree; skips an already-applied (empty) pick and says so. **Exit 2 = conflict**: it prints the SHAs not yet picked and the exact `pick_stage.sh --continue <same flags> <rest>` line; resolve, `git add`, run it (it finishes the pick, strips the `-x` line, checks the trailer, writes the note, picks the rest). **Exit 3 = trailer violation**: fix the trailer on the lane branch, `git reset --hard HEAD~1` on staging, re-pick. |
| Before a gate, or on a FAIL | `scripts/pick_ledger.sh [RANGE]` | the ledger above: one row per commit, per-pool commit and fix-issue counts (only kind=fix commits bank an issue, credited to the pool of its earliest fix commit; with no recorded kind it is inferred from the subject and marked `*`), and the UNATTRIBUTED count (no note by SHA or by patch-id). |
| Dispatching a lane | `scripts/lane_setup.sh --owner <name>` or `scripts/wt_owner.sh claim <name>` | nothing: another session's lane once switched a worktree it did not own to its own branch. With the hooks installed (`scripts/install_hooks.sh`, once per repo, a coordinator decision; it acts on the repo of the **current directory**), a commit in an owned worktree on a different named branch, or while rebasing a different branch, is refused, and switching to one prints a warning. A detached HEAD (bisect, `switch --detach`, the owner's own rebase) is allowed. `BEZEL_OWNER_OVERRIDE=1` bypasses it. |

**Rebasing staging drops the notes** unless git is told to carry them. The
primary path — the only one that is reliable — is, per command (never
`git config`):

    git -c notes.rewriteRef=refs/notes/bezel-picks rebase <onto>

If that was forgotten, `pick_ledger.sh` falls back, **best effort**, to the
pre-rebase commit: a noted commit with the same `git patch-id --stable` *and*
the same subject. Exactly one such commit → its note is used and the row's
kind is marked `~` (e.g. `fix~`), counting as recorded. It never guesses
between candidates. A row stays UNATTRIBUTED, with its kind inferred and
marked `*`, when:
- **the patch changed** in the rebase (a conflict resolution), so no patch-id
  matches;
- **duplicates are ambiguous**: noted commits share the row's patch-id but
  none, or more than one, has its subject (a fix, its revert and a re-add
  under another subject have identical diffs; the same subject picked twice).
  LANE then reads `(ambiguous)`;
- **the pre-rebase objects were pruned** by `git gc` / `git prune`: a note
  outlives the commit it annotates, and a pruned target can no longer be
  matched (it is skipped, so the other notes still recover).
Recovery computes one patch-id per note in `refs/notes/bezel-picks`, so its
cost grows with the notes ref; nothing prunes that ref yet.

Limits worth knowing:
- notes are local refs and are not pushed, so the ledger lives on this machine;
- git 2.55 runs `pre-commit` for `commit` and `commit --amend` but **not** for
  `cherry-pick`, `rebase` or `merge`, so the ownership hook guards the ordinary
  commit path only;
- issue credit is the `#NNN` in the subject, so a subject that cross-references
  another issue (`fix #A (follow-up to #B)`) credits both; the fallback kind
  inference is a prefix heuristic, which is why rows it classifies carry `*`;
- handback_check's `named-tests` row greps test-like names out of commit
  messages, so a name mentioned in prose can WARN falsely. Read it, don't
  count it.

## Autonomous run discipline (lanes, gates, tests)

These rules are not style preferences — every one was paid for in a lost gate
cycle, a shipped regression, or hours of dead wall-clock. They bind **lane
agents and coordinators alike**; a lane brief should assume them rather than
restate them.

### Never weaken a test to make it pass

A test oracle encodes the spec's expected behaviour. Lowering its bar silences
the bug instead of fixing it, and **a green test with a weakened oracle is worse
than a red test with a strict one** — the red one is actionable, the green one
hides a regression that resurfaces as a crash. Forbidden, without exception:
turning `EXPECT_EQ` into `EXPECT_NE`, relaxing `exitCode == 0` to `!= 0` or
`== 99`, flipping `ASSERT_TRUE(leaksAtLeast(p, N))` to `ASSERT_FALSE`, deleting
`didNotCrash` / `assertNoLeak` markers, or adding a `DISABLED_` prefix. A fix
that "passes" this way is **reverted and re-dispatched**. Precedent: a lane
weakened a `runChecked == 0` row to `leaksAtLeast(prog, 11000)` and justified it
as "the post-fix safe leak is correct" — which is exactly the rationale that
hides a use-after-free. *Strengthening* an oracle is always welcome; if you
believe an oracle is wrong, say so and stop — do not quietly change it.

⚠ The rule cuts **both** ways. A fix that reverses behaviour (admits what was
rejected, or rejects what was admitted) **must** update the tests that pinned the
old behaviour — the gate's REAL-vs-FLAKY arbitration re-runs a failing test alone
5× and classifies a deterministic-but-now-obsolete test as REAL even though the
compiler is right. One cycle burned 5 regressions this way. Before committing,
`git grep` every test named in the commit message and every test in the files
touched; name the flipped tests and their new expectation in the commit body.

### A regression test must pin THE BUG, not the neighbourhood

Every fix ships a spec-cited test that **fails on the pre-fix code and passes
with the fix**, and you must prove FAIL-WITHOUT mechanically rather than assume
it. A test that merely exercises the same area gives no protection: a spec-cited
gtest for a visited-set-on-walk fix once used a non-cyclic 2-level diamond that
never revisits a node, so deleting the fix would not have failed it. The input
must be the issue's actual repro — assert termination for a hang, no-crash for a
crash, the corrected value for wrong-code, acceptance for an over-rejection,
dispose-count/ASan-clean for a double-free.

### Oracles that pass vacuously

- **One-shot leak repros.** LSan reports *unreachable* allocations; a single run
  can leave the allocation reachable from a live stack slot or register, so the
  row passes on both sides of the fix. Measured: clean one-shot on fixed and
  unfixed compilers; loop-amplified to 500 iterations it showed 18,500 B / 1,000
  allocations pre-fix and clean post-fix. **Ship the loop** (50–500 iterations).
- **`failedAtCompileTimeWithRealLocation` on a crash.** The crash's own backstop
  diagnostic carries a real location, so the helper returns true on both sides.
  When the pre-fix behaviour is a crash/IR-verifier failure/signal, assert the
  *specific* diagnostic. (Caught because FAIL-WITHOUT showed 2 of 4 rows failing.)
- ⚠ **Generated-code ASan is a GATE-ONLY oracle, so a lane's green does not
  cover it.** `BEZEL_AOT_FORCE_SANITIZE=address` is exported by `gate.sh` and
  `gate_full_cycle.sh` and by **nothing else** — `agent_check.sh` does not set
  it (verified 2026-09-22). So `runAot`/`runAotChecked` rows a lane runs are
  NOT instrumented, and a defect that only faults under ASan is invisible until
  the gate. That is not a silent skip and not a vacuous oracle — the lane's run
  is honest about what it measured; it simply measured the plain build.
  Measured cost: #4579 shipped into a gate this way. Its authoring lane ran 8/8
  green (truthfully), and the row stack-buffer-overflowed on the gate's `--full`
  leg, because a LinkedList insert allocated a 4-byte slot and told the runtime
  the stride was 8. **If your change touches allocation, element layout,
  release/retain, or anything a sanitizer could catch, re-run your own filter
  with the env var set** before handing back:
  `BEZEL_AOT_FORCE_SANITIZE=address scripts/agent_check.sh --area codegen <filter>`
  and confirm the run prints `NATIVE SANITIZER: 0 check(s) skipped`.
- **The `-1` sentinel silently deletes the ASan leg — but NOT for the reason
  this file used to give.** ⚠ The old wording here said "the native ASan leg is
  vacuous in lane worktrees, `asanRuntimeObject()` is empty there" and told you
  never to accept a lane-local ASan green. **That is false and was costing real
  evidence.** `asanRuntimeObject()` (tests/native_test_harness.hpp) does not look
  up a prebuilt artifact — it *compiles every runtime TU under
  `-fsanitize=address` and `ld -r`-combines them*, memoized per sanitizer, so it
  works in any worktree with a toolchain. Measured 2026-09-22: 285 lane runs, all
  reporting `NATIVE SANITIZER: 0 check(s) skipped`, with the objects on disk under
  `/var/tmp/bezel-scratch/rtobj/`. A lane-local ASan green IS evidence.
  The real hazard is narrower and sharper: the `runUnderAsan*` helpers return
  `-1` for **toolchain unavailable, compile failure AND link failure alike**, and
  every caller writes `if (rc == -1) return;`. So a *link* break makes the
  sanitizer leg vanish from every row in that file and report exactly the same
  green — and `nativeSanitizerUnavailableSkips()` (#3626) counts only the first
  of the three, so a suite-wide outage of the other two is invisible to the gate
  (#4563). Confirm the leg RAN — `0 check(s) skipped`, or `nm` the emitted object
  for `__asan_report*` — rather than assuming it did or assuming it didn't.
- **"Compile it N times" is blind to hash-ORDERING defects.** Bucket sharing
  depends on pairwise address *differences*, and ASLR shifts every allocation by
  a constant — so 40 compiles yield one identical object even when the ordering
  is genuinely defective. The oracle buys zero; don't specify it.
- **8-bit exit truncation is not a compiler bug.** `runAot`'s `exitCode` is
  `WEXITSTATUS`-masked: 298 reads as 42, 1006 as 238. Never assert a value above
  255 — move the observable to stdout.
- **"Pre-existing" needs a named baseline.** If the lane measured against a
  mid-batch commit it has only shown "*my* fix didn't cause this". The baseline
  must be `origin/main`.

### Lane hygiene

- **`cd` into your worktree as the literal first action**, and never leave it.
  All writes, builds, `git` commands and test files go there. Lanes have written
  regression tests into the shared checkout because that is the path of least
  resistance; the brief must make the `cd` a precondition, not an after-the-fact
  `git status` check.
- **Never `git stash`.** `refs/stash` is shared across all worktrees of a repo,
  so one lane's `pop` can silently take another lane's uncommitted changes — this
  happened. To test fail-without, copy the file to `/var/tmp/bezel-scratch`,
  `git checkout -- <file>`, rebuild, then restore. Same hazard for
  `git checkout <rev> -- src/` inside a revert-proof script: killing it mid-run
  leaves the tree at the old revision.
- **A lane never runs the full suite** (`run_parallel` / `check`) — that is the
  gate's job. Use `scripts/agent_check.sh [--area <group>] <filter>`. Five lanes
  each cold-building their own test group once put 77 concurrent `clang++` in an
  18-core slice, pinned ccache at 80G with 7,600 evictions, and left one lane at
  9% after 45 minutes with zero `error:` lines — which reads exactly like a build
  failure and is not one.
- ⚠ **Rebuild the TEST binary, not just `bezelc`.** `runAot`/`runAotChecked` do
  **not** exec `build/bezelc`; they fork an **in-process `Driver::compileFile`**
  linked into `bezel_tests_<area>`. So after any `src/` edit **or any
  cherry-pick**, a `bezelc`-only rebuild leaves the tests running the OLD
  compiler. This has twice produced a confident wrong diagnosis (~30 min and
  ~15 min). Assert no source is newer than the binary before trusting a result.
  Equally, `git checkout <ref>` does **not** rebuild `bezelc` — a bisect that
  skips the rebuild measures the same binary at both ends and concludes
  "pre-existing".
- **Never run two `cmake --build` on one build dir** — make does not lock, both
  schedule the same targets, and killing them leaves a tree the gate refuses.
- **Cherry-pick from inside the integration worktree**, not the main checkout —
  the integration branch is generally *not* what `main` has checked out, and a
  cherry-pick run from the wrong cwd lands on `main`. Confirm with
  `git branch --show-current` before every pick.
- Scratch, worktrees and build dirs never go in `/tmp` — it is a 16 GB tmpfs with
  a uid quota, and exhausting it makes **every** Bash call return exit 1 with no
  output, which looks exactly like a harness failure. Use
  `/var/tmp/bezel-scratch/` or `/mnt/fast/`.
- Uncommitted WIP recovered from a crashed run is an **unverified hypothesis**,
  not work that is 80% done. Re-derive the diagnosis from the issue and HEAD:
  5 of 5 inherited WIPs were once defective, and two would have shipped a bug.

### ⚠ At most TWO lanes may cold-build `--area codegen` at once

Measured 2026-09-23 by **three independent lanes in one cycle**, all reporting the same
thing: a `--area codegen` cold build in a lane is **not survivable** when several siblings
are doing the same.

- one lane's build killed with **161** concurrent `cmake`/`clang++` observed;
- one `Terminated` by the slice at **33%** with 6 lanes building and 2 GB of swap in use;
- one measuring **92** concurrent `clang++` in an 18-core slice, progressing at ~4 TUs per
  poll against 1,532 remaining, after an earlier attempt was SIGTERM'd at a 1.7 GB peak.

**The kill looks exactly like a build failure** — `gmake … Terminated`, no `error:` lines —
which is the same signature as genuine contention noise and as a real compile error. Two of
the three lanes lost verification work to it.

**The constraint is not the lane count, it is concurrent cold builds of the 212 MB
`bezel_tests_codegen` binary.** `--area sema`, `module`, `frontend` and `support` are ~50 MB
and are cheap by comparison; a dozen lanes doing those cost little.

Rules:
- **Cap concurrent `--area codegen` lanes at 2.** Stagger the rest; a lane that will build
  codegen should wait rather than start a third.
- Spread a wave across areas deliberately — do not dispatch six codegen lanes because six
  issues happen to be codegen issues.
- When a lane reports a build "failure" with **zero `error:` lines**, suspect the slice
  before suspecting the code, and check `ps -eo comm= | grep -cE '^(clang\+\+|cc1plus)$'`
  before re-running.
- A lane that stops its own build to return slots to siblings further along is doing the
  right thing, not giving up — one lane did exactly this and said so.

### The orchestrator MUST run a CronCreate heartbeat

**An autonomous run's coordinator schedules a recurring heartbeat (CronCreate) for the
whole run, as its first action.** Without one the coordinator can go silent for hours: it
launches a long job detached, nothing wakes it, and the run stalls with the machine idle.

Measured 2026-09-23: a consolidated pre-gate sweep was launched with
`setsid nohup … &` and **no waiter**. It finished in minutes; the coordinator did not look
again for **~3 hours**. The sweep had also reported a failure, so the gate was blocked that
whole time. Nothing was wrong with the build, the lanes, or the machine — only with the
coordinator's own wake-up path.

Two mechanisms, and they are not interchangeable:

1. **Per-job waiters** — the primary. Launch long work so its completion re-invokes you
   (a background Bash job, or `scripts/wait_pid.sh "$PIDFILE"` on an identity, never a
   `pgrep` pattern). This is precise: you wake when the job ends, not on a timer.
2. **A CronCreate heartbeat** — the backstop, for everything a waiter cannot cover: a job
   that dies without writing its marker, an uncatchable kill, a waiter that was never set,
   or a coordinator that simply stopped having a reason to run. Schedule it once at run
   start and let it fire for the run's duration.

⚠ A heartbeat is a **backstop, not a scheduler**. Cycle 9 lost ~12 h of a run when a
misfiring cron hit the hard-stop early and left the ASan oracle INCOMPLETE — so the
heartbeat must only *prompt a status check*, never take an action (never launch a gate,
never push, never dispatch). Keep its interval coarse (tens of minutes); a tight interval
burns budget re-reading state that has not moved.

### Gate protocol

1. **Never launch a gate you expect to fail.** Run every targeted filter for
   every known regression on the candidate HEAD first; if any FAILs, fix forward
   and re-check before launching. A gate you know will fail is performative and
   creates push pressure that leads to weakened oracles.
2. **Zero agents for the gate's full duration (~50 min)** — no fix, review or
   probe agents, and no replacements dispatched. Read-only work is fine. "It's
   just reading code" is a snapshot, not a guarantee: an agent can move to
   "needs to rebuild" on any turn.
3. **Zero builders** — check it with **`scripts/no_builders.sh`**, never by hand.
   ⚠ The recipe this line used to give, `pgrep -x 'clang++|clang|cc1plus|...'`, is
   **broken in the one direction that matters**: `pgrep -x` matches the process
   name as an anchored extended regex, so `+` is a metacharacter and
   `pgrep -xc 'clang++'` returns **0 while 31 clang++ processes are running**
   (measured 2026-09-21). That alternation only ever fired via `cmake`, so a box
   compiling hard with no cmake alive reported CLEAN. A precondition that cannot
   fail is worse than none — it is the same vacuous-oracle class as a one-shot
   LSan row. The script matches on `ps -eo comm=` with fixed strings and fails
   closed. Contention is indistinguishable from a real regression: one
   concurrent builder produced "shard crash / undercount — NOT trustworthy" plus
   53 phantom failures that all passed in isolation. If a FAIL carries the
   undercount banner, re-run each suspect alone with `BEZEL_TEST_FILTER=<substr>`
   before believing it. Note the zero-builders preflight guards only the **ASan**
   leg; the plain leg has no such refusal, which is why the agent drain must be
   preventive.
4. **Pre-gate ratchet sweep** — build-free, fails closed. Running these by hand on
   one batch found three RED that would each have failed the cycle. ⚠ **Derive the
   list from the gate; do not hardcode it.** A hardcoded six-item list drifted out of
   date and silently omitted `check_toolchain_skip_surface`, which the gate treats as
   a `CYCLE FAILURE` — a ~50 min cycle would have burned on a known-fail:
   Run **`scripts/ratchet_sweep.sh`**, which derives the list from the gate and
   fails closed if it derives none. Equivalent by hand (never a hardcoded list):
   ```sh
   for s in $(grep -oE 'scripts/check_[a-z_]+\.py' scripts/gate_full_cycle.sh | sort -u); do
     printf '%-42s ' "$(basename "$s" .py)"
     python3 "$s" --strict >/dev/null 2>&1 && echo PASS || echo FAIL
   done
   ```
   ⚠ **Adding `runAotChecked` rows legitimately raises `checked_calls` and turns
   `check_toolchain_skip_surface` RED.** That is a *denominator* rise, not a new
   silent-skip site. Before `--update --force`, verify what the ratchet's own
   diagnostic asks: the new rows carry no `wasSkipped()`/`find("SKIP")` guard, no
   early return, assert `r.ran` unconditionally, and `absorbing_sites` did **not**
   rise. Bank it **once at batch level**, not per lane — several lanes adding checked
   rows would otherwise all collide on the single baseline file.
5. **No untracked `scripts/`** — ratchet #3413 fails the cycle on any `.sh`
   hardcoding an absolute checkout path. Park strays in
   `/var/tmp/bezel-scratch/<run>/parked/`.
6. **Check the fast-disk symlinks.** Both `/var/tmp/bezel-scratch/aot-bincache`
   and `.../aot-harness` must be **symlinks** into the fast disk. If either has
   been replaced by a real directory the gate runs on slow storage —
   `scripts/lane_setup.sh` refuses to clobber a real dir, so it silently does
   nothing in that state. Observed: bincache a real dir on sda3, 47% hit rate,
   codegen shards at 75–93% of the 1800s ceiling. Watch `df -h /mnt/fast` too.
7. **A reboot or hard kill leaves a stale pidfile**, because it is removed in an
   EXIT trap — the next gate may then refuse with "another gate is running".
   Confirm the recorded PID is really dead (`kill -0`) before removing it.
8. ⚠ **systemd-oomd can kill your terminal — and the gate with it.** Fedora's
   `/usr/lib/systemd/user/slice.d/10-oomd-per-slice-defaults.conf` sets
   `ManagedOOMMemoryPressure=kill` at 80% on **every** user slice, and KDE sets it
   on each transient konsole scope (with `Limit=0`, i.e. oomd's 60% default). When
   a gate build drives memory pressure, oomd kills the *highest-pressure child of
   `app.slice`* — which is an interactive terminal tab, **not** the build. The tab
   is not killed for being large (2.1 GB when it happened); it is collateral damage.
   Measured 2026-09-20: three konsole tab scopes killed (16:20:34 ×2, 21:35:45). The
   21:35:45 kill took a Claude session **and the gate orchestrator** with it — SIGKILL,
   so no EXIT trap ran, leaving a stale `gate.pid`, no verdict, no ASan leg, and
   orphaned shard workers that ran to completion in `bezel-heavy.slice` and wrote a
   log at 22:06. The symptom reads as "the session crashed" plus "the gate vanished".
   `setsid` does **not** protect against this: it changes the session, not the cgroup.
   Mitigations, all applied: opt `bezel-heavy.slice` and `bezel-probe.slice` out with
   `ManagedOOMMemoryPressure=auto` (the `MemoryMax` cage is the real bound and is more
   precise than an oomd slice kill); keep probe work **out of `app.slice`**; and
   opt the terminal's own scope out at the user level.

On FAIL: **never revert, never bisect** — narrow fix-forward only, and keep the
pool at zero agents until the fix lands and the targeted filters are green.

### Pool size and model choice

Fan-out is the standard mode, not an escalation: the build/test loop is ~2 min
while most of the per-merge time is serial cognition. Run a **firm N=6** agents
in flight (producers + reviewers) and refill a slot in the same turn it frees —
except during a gate, when it is zero. Keep lanes **file-disjoint**: no two on
the same hot dispatch function (`typeCall`, `lowerBinary`, the `tryLower*Call`
family), and at most one disposal/ownership/async/iterator lane at a time.
Bundle at most 2–3 closely-related issues per lane, one commit per issue.

**Sonnet is the default; dispatch Opus UPFRONT** — never as a fallback after a
Sonnet attempt came back short — when the task spans ≥2 predicates across files,
needs nested closure/captured-cell reasoning, is a 3+-issue cluster with one root
cause, has a suspect root cause, follows a *refuted* partial fix, or is **any
gate-failure fix-forward** (always Opus). Judge difficulty, not topic. If a
Sonnet fix regresses more than it fixes, revert it and re-dispatch as Opus.

### Commits

End every commit message with exactly one trailer line and nothing else — no
`Signed-off-by`, no second trailer:

```
Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
```

Cite the spec section **and** revision in the body (`§28.2, v0.303`); the gate
runs `scripts/check_src_citations.py` and every `§`-citation in `src/` must
resolve. `narrow #N` is the partial-fix convention — but a closing keyword in a
**sibling commit's body** still auto-closes the issue, so `git log --grep="#N"`
before assuming work is outstanding. An OPEN issue is not evidence the work is
undone, and the open count is not a metric.

