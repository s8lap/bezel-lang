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
`docs/ops/lane-contract.md`; a peer coordinator reads `CLAUDE.md` and
`docs/ops/peer-contract.md`. They do not need this file unless a rule's reason
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

⚠ **"Built target X" is not "X was built".** A `cmake --build` that prints
`Built target bezelc` may not have relinked it — the gate's staleness
preflight names the "#2426 unreliable-incremental family", where the build
reported success and the binary was unchanged. So after any `src/` change, the
only sound proof that the binary is fresh is a **behavioural probe of the
change** (does it still reject / still accept?), not the build's exit status
and not an mtime. And a coordinator-side build runs in the **foreground**: a
backgrounded build that is killed leaves a tree that certifies a stale binary.

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
tree parked there permanently consumes gigabytes. Keep ccache at about 80G on
disk (a full 40G cache thrashes under multi-worktree churn). Since 2026-10-01
that is `max_size = 180G` with `file_clone = true`. Clone hits and stores write
no object data, since they reflink on the btrfs NVMe. Cloned entries are stored
uncompressed, and btrfs reports uncompressed sizes, so `max_size` counts
uncompressed bytes. Measured: ccache's own compression was 2.3×, so 180G holds
the entries the old 80G did.

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
**The cap is `N + #builds`, not `N`** (r38-i1-jobserver): every joined build
keeps ONE implicit job slot — the slot its first job runs in, without taking
a token — so the box can run 20 tokens PLUS one job per joined build. That is
why a one-TU relink never queues behind a cold build, and it is why the
coordinator caps COLD builds at TWO: two cold builds plus 20 tokens is already
22 concurrent compiles, and every extra cold build adds another implicit slot
on top of the same cores. A CMake build nests several `make` levels and each
level keeps its own implicit slot, so `builds` in the status line counts make
PROCESSES (a nested CMake build shows 4–9), not build PROJECTS.
Two layers enforce this since 2026-09-30, when a coordinator's lane briefs said
`cmake --build build --target bezel_tests_sema -j` (unlimited `make -j`, outside
`dev_heavy.sh`): 499 clang, and the OOM killer took three Claude sessions and
plasmashell. (1) Every generated compile and link runs through
`scripts/build_token.sh`, which takes a pool token unless its make/ninja already
joined the pool, or one of 16 machine-wide flock slots when no launcher-aware
daemon runs. A bare `make -j` therefore queues instead of fanning out, however it
was started. (2) A Claude Code PreToolUse hook (`scripts/claude_hooks/build_guard.py`,
installed as `~/.claude/hooks/bezel_build_guard.py` in `~/.claude/settings.json`,
with one `--root` each for the main checkout, `/var/tmp/bezel-scratch` and
`/mnt/fast`, which holds the lane worktrees; it acts only on a session cwd or a
command under a root) refuses a bare `cmake --build`/make/ninja in a build dir, a `BEZEL_JOBSERVER=off`
or `MAKEFLAGS=` override, and an Agent prompt or `*brief*` file that tells a lane
to run one, and names the command to run instead. Denials are logged to
`/var/tmp/bezel-scratch/build-guard/denials.log`. After changing the hook, re-copy it.
(3) OpenCode sessions have no `~/.claude/settings.json` hook, so
`scripts/claude_hooks/opencode_build_guard.js` is installed as
`~/.config/opencode/plugins/bezel-build-guard.js` and asks the SAME rule set
what to do with each shell command (`build_guard.py --route`, one subprocess per
call). It ROUTES instead of refusing: a bare `cmake --build …`, or `make`/`ninja`
in a CMake build dir, is rewritten to `scripts/dev_heavy.sh build <tool> …` —
the same build dir, the same targets, the same output paths, but joining the
token pool and the memory-capped slice. A build the rewriter cannot see (a
wrapper prefix, a `$( )` payload) keeps its own words and only gets the
jobserver env (`MAKEFLAGS` with the FIFO auth, `BEZEL_JOBSERVER=on`, no
`CMAKE_BUILD_PARALLEL_LEVEL`), so it still takes a token per compile. Only two
things are refused: a pool opt-out (`BEZEL_JOBSERVER=off`, a hand-set
`MAKEFLAGS`, `--jobserver-auth`) and a by-name kill — neither can be routed, and
a kill is not a build. Refusing a build costs a lane a round-trip, and an agent
that does not understand the refusal retries with a slightly different bare
build; routing keeps the lane's intent and changes only the mechanism. The
route mode is pinned by `scripts/gate/tests/test_build_guard_route.py`, the
deny mode by `scripts/gate/tests/test_build_guard_hook.py`. After changing the
hook, re-copy it to BOTH `~/.claude/hooks/bezel_build_guard.py` and
`~/.config/opencode/plugins/bezel-build-guard.js`.
`start` refuses to run from a linked worktree. A *dedicated* daemon worktree (e.g.
`/mnt/fast/wt-jobserver21`) needs `BEZEL_JOBSERVER_ALLOW_WORKTREE=1`. The unit name is
`bezel-jobserver` for the default token dir, and `bezel-jobserver-<hash8>` for any other
`BEZEL_JOBSERVER_DIR` (override with `BEZEL_JOBSERVER_UNIT`, #4990).

**Reading the jobserver status line** (r38-i1-jobserver). Every field is
OBSERVED — the daemon used to report a GUESS (`held~`: "a joined make with k
children holds k-1 tokens"), which was wrong whenever a build was not running
at its peak concurrency: a build running its LAST job holds one token with one
child, the guess read that as zero, and the daemon minted a token that was
really held (the pool went to 21, one compile over the cap) and then drained
the surplus again. Measured on the production pool, `samples.tsv` rows 9961-9963:
`available` 19 → 21 → 21, `repaired` +1 then -1, with a compile in flight the
whole time. The line now reads:

```
build_jobserver: available=0 held=20 tok_held=20 tok_wait=10 builds=1 jobs=21 clang=25 tokens=20 pid=… fifo=…
```

| field | meaning |
|---|---|
| `available` | tokens physically in the FIFO (`FIONREAD`) — the pool's occupancy, the ground truth |
| `held` | pool size − `available`: the tokens checked OUT (negative ⇒ the FIFO holds more than the pool: a surplus) |
| `tok_held` | launcher processes holding one token each (a job that HAS a slot) |
| `tok_wait` | launcher processes QUEUED for a token (a job waiting for one) |
| `builds` | joined `make`/`ninja` processes holding the FIFO; each keeps one implicit job slot |
| `jobs` | the box-wide cap: `N + builds` |
| `clang` | live compiler processes, measured at the same instant (a `clang` far above `held`+`builds` is the leak signature) |

So "the tokens are actually being used" is VISIBLE: 30 concurrent compile jobs
read as `available=0 tok_held=20 tok_wait=10` — the pool drained and the
excess queued — and a build running only its implicit slot reads as
`available=20 held=0 builds=1 jobs=21` (the pool is idle, but a build is
running; those are different facts and the line now says which). The audit
re-mints a token only when the shortfall is PROVABLE — nobody can be holding
it: no launcher holds it and no joined build could be holding it — and drains
one only when the FIFO itself holds more than the whole pool. The regression
test is `scripts/gate/tests/test_build_jobserver_accounting.py`; it fails
against the pre-fix daemon (`BEZEL_JOBSERVER_SCRIPT=<pre-fix copy>` proves it).
⚠ The daemon is a long-running systemd unit started from the MAIN checkout:
after a change to `scripts/build_jobserver.py` lands there, the coordinator
must `systemctl --user restart bezel-jobserver` — the running daemon keeps
executing the code it was started with.

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
for this class of mistake. Cleanup is automated **only on a push**; a
run that has not pushed reaps by hand (see *Reaping stale processes, worktrees and build
dirs*): `scripts/gate_and_push.sh` runs
`scripts/gc_scratch.sh` after every successful push, which (a) reaps worktrees
whose commits are already in `origin/main` (branch refs are kept; WIP, locked,
and the current worktree are never touched), (b) drops stale
`/var/tmp/bezel-scratch/build-asan-*` trees, and (c) quarantines >48h-old root
scratch into `/var/tmp/bezel-scratch/root-quarantine/` (moved, not deleted;
self-purged after 14 days). Run `scripts/gc_scratch.sh` (dry-run) any time to
preview, `--apply` to act; `scripts/reap_orphans.sh` clears processes a dead
agent orphaned. Lane worktrees live in `/mnt/fast/worktrees/<name>` (see *Lane
hygiene*) with a real in-tree `build/`, which `git worktree remove` deletes along
with the worktree. ⚠ **A worktree on another disk keeps its build dir outside the
worktree.** For one of those, `scripts/lane_setup.sh` symlinks `build/` out to
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

### Shared read-only builds (reviewers and verifiers do not build)

ccache shares compiles across worktrees, but every lane still links and stores
its own `bezelc` plus six test binaries (~3.6 GB a build dir), and a reviewer
builds two arms. `scripts/shared_build.sh` builds a commit ONCE and publishes it
read-only under `/mnt/fast/shared-builds/<full-sha>/` (a locked, detached
worktree at that sha with its `build/`, plus a `MANIFEST` written last: sha,
tree key, toolchain, build time, md5s). The newest 4 are kept; `prune` never
removes one a reader holds.

- **Coordinator: publish after each pick batch**, and before dispatching the
  reviewers and verifiers that will run against it:
  `scripts/shared_build.sh publish` (default ref: `$BEZEL_STAGING_REF`, else
  `r30-c1`; `--ref R` for another, e.g. a review's BASE arm). It builds through
  `dev_heavy.sh`, waits for 6 free jobserver tokens, and uses `-j 4`.
  `scripts/shared_build.sh ensure R` is the same but returns at once when R is
  already published; two lanes ensuring one sha build it once (the second
  waits on an flock). `path R` / `list` only look.
- **Reviewers / verifiers** (read-only lanes, clean tree):
  `scripts/agent_check.sh --use-shared <ref> [--area A] <filters>` and
  `scripts/agent_check.sh --probe [--asan] FILE.bzl --use-shared <ref>` run the
  published binaries with NO build — seconds instead of a link. A review's tip
  arm, built from the lane branch, is still a normal build; only arms that
  match a published commit are shared. `--use-shared auto` takes this tree's
  HEAD and accepts any published commit with the same tree key (sha1 over
  `git rev-parse <sha>:src`, `:tests`, `:CMakeLists.txt`, `:cmake`), so a
  commit that changed only docs or scripts reuses its parent's build.
- **It refuses** (exit 3, with the changed paths) when the worktree's `src/`,
  `tests/`, `CMakeLists.txt` or `cmake/` differ from the ref, tracked or
  untracked: the published binaries were built from the ref's sources, and the
  run would otherwise report on code the lane is not looking at. `--lit` is
  refused too (lit writes into the build tree). Without the flag nothing
  changes; a plain run on a clean tree with a published match only prints a
  NOTE naming the build.
- **Why the build is not copied.** The test binaries bake five absolute paths
  (`tests/CMakeLists.txt`, the generated `build_paths.cpp`: the runtime and
  core archives, `bezelc`, `src/runtime`, the repo root) and `Driver.cpp`
  bakes the runtime archive's path, none with a run-time override. A copied
  binary keeps using the tree that built it, so each sha is built in its own
  worktree at its final path, and the MANIFEST-last rename is the atomic step.

## Dual-coordinator runs (two sessions, one repo)

A run may be driven by **two coordinator sessions** sharing one repo, one machine
and one `origin/main`, each with its own pool. That configuration has run for
several cycles and is productive — the serial bottleneck is coordinator
cognition, and two of them genuinely double it. Every rule below was paid for in
a lost gate cycle or a corrected memory file during those runs.

⚠ **None of this was written down until 2026-09-24.** The protocol lived entirely
in one coordinator's private memory directory, which lane agents never read and
which the two coordinators had already collided on. If you are the second session,
start from `docs/ops/peer-contract.md`: it is this section's rules as a work loop.
This section keeps their reasons.

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

**The two conformance corpora are a third and fourth track, and no gtest/lit
needle grep reaches them.** `tests/conformance/` holds the doc examples and
`tests/conformance/rules/` the rule cases. A rule case's `stdout:`/`output:`
header and a doc-example row's `run:<first line>` expectation are needles in
exactly the sense a gtest string literal is, and neither is found by grepping
`tests/**/*.cpp` or `tests/lit/**`. A flip that changes what a program PRINTS
breaks both. A flip that changes a diagnostic's WORDING breaks neither — the
wording of a reject or warn diagnostic is deliberately not gated (`runner.py`:
*which wording is the diagnostic report's business, not the gate's*) — but
adding or removing a diagnostic breaks a `quiet` case, and removing one breaks
a `warn` case, or a `run` case carrying a `warns:` header.

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
| Every heartbeat | `scripts/run_status.sh [--staging <wt>] [--target N]` | eight hand-typed probes: gate pidfile/donefile/cycle log, jobserver unit, builders, MemAvailable + swap-out, `/mnt/fast` use and the aot symlinks, staging count vs target, lane count. Prints `ACTION:` lines only for flagged rows; never acts. **The builders row carries TWO readings:** the instantaneous `no_builders.sh` verdict (the GATE PRECONDITION — unchanged, because "is anything building NOW?" is the question a gate must ask) **and** a 5-second window (`5s window: N/5 samples saw a builder`). The window exists because a single-instant sample of a box that alternates between building and analysing reads "CLEAN" while every lane is mid-run — measured 2026-11-02 23:33:13, zero build processes at the sample and three five seconds later. **The lanes row carries the live per-lane build-process count and the box-wide heavy-process count** for the same reason: "4 active lane worktree(s)" says nothing about whether any of them is building. |
| A lane hands back | `scripts/handback_check.sh <lane-branch> [--base REF] [--reported-sha S] [--grant G,…] [--author 'N <e>']` | the manual handback checklist: reported SHA is the tip (else: in the branch, elsewhere, or dangling), base is an ancestor, trailers, `git config` author leak, files outside the grant, `/home` paths in new scripts and hooks, stale needles, tests named in commit messages exist. Read-only (pinned by a test); FAIL exits 1, WARNs are for the reviewer. It does not replace the non-author review. **Mid-batch, measure against staging:** `--base <staging-branch>`, or export `BEZEL_STAGING_REF=<staging-branch>` once (the default when set; else `origin/main`). |
| Picking onto staging | `scripts/pick_stage.sh --branch … --pool … --lane … --reviewer … --verdict ACCEPT\|ACCEPT-W-NOTES --kind fix\|test\|bank\|infra SHA…` | the scratch pick script and the hand-kept batch ledger. Records pool, lane, orig SHA, reviewer, verdict, kind and issues (the `#NNN` in the subject only) as a note under `refs/notes/bezel-picks`. Refuses a dirty tree; skips an already-applied (empty) pick and says so. **Exit 2 = conflict**: it prints the SHAs not yet picked and the exact `pick_stage.sh --continue <same flags> <rest>` line; resolve, `git add`, run it (it finishes the pick, strips the `-x` line, checks the trailer, writes the note, picks the rest). **Exit 3 = trailer violation**: fix the trailer on the lane branch, `git reset --hard HEAD~1` on staging, re-pick. |
| Before a gate, or on a FAIL | `scripts/pick_ledger.sh [RANGE]` | the ledger above: one row per commit, per-pool commit and fix-issue counts (only kind=fix commits bank an issue, credited to the pool of its earliest fix commit; with no recorded kind it is inferred from the subject and marked `*`), and the UNATTRIBUTED count (no note by SHA or by patch-id). |
| Dispatching a lane | `scripts/lane_setup.sh --owner <name>` or `scripts/wt_owner.sh claim <name>` | nothing: another session's lane once switched a worktree it did not own to its own branch. With the hooks installed (`scripts/install_hooks.sh`, once per repo, a coordinator decision; it acts on the repo of the **current directory**), a commit in an owned worktree on a different named branch, or while rebasing a different branch, is refused, and switching to one prints a warning. A detached HEAD (bisect, `switch --detach`, the owner's own rebase) is allowed. `BEZEL_OWNER_OVERRIDE=1` bypasses it. |
| "Is this commit's content already in the tip?" | `scripts/pick_presence_check.py <tip-ref> <commit>…` | a hand-rolled presence check, which at R36 built the tip's file dict from a FIXED list of four files and so reported `MISSING FROM TIP=0` for a commit in the list's blind spot — a FALSE "already picked", on the very commit that was the GATE BLOCKER. This script builds the file set from the commits themselves and makes an unknown file FATAL (exit 2). `--self-test` pins that incident. |

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

**A merge of `origin/main` into staging needs BOTH a trailer and a ledger
note.** The trailer half is enforced (`check_commit_trailers.py` runs over
`rev-list origin/main..HEAD`, so a merge commit with none is caught); the note
half is not, and a merge without one shows up as UNATTRIBUTED in the ledger. So
a merge commit gets a trailer naming the model that wrote it, and a note under
`refs/notes/bezel-picks` like any other commit. `git commit --amend -F` keeps
both parents, so the note survives a subject fix — but if the SHA changes, the
note has to be re-added for the new SHA.

**A checker's verdict is not evidence about the tree you are standing in.**
Three habits, each of which has cost a wrong conclusion:
- **Read the instrument's own header before choosing the interpreter and the
  flags.** `corpus.py --strict` is not a valid invocation and exits 0.
- **Capture the exit code BEFORE piping.** `cmd | tail; echo $?` reports
  `tail`'s status, not the tool's.
- **Print the count, not just the verdict.** An empty revision range is an
  error, not a result; "0 problems" from a checker that was handed no file is
  not a pass.

**Amending is where a reviewed commit can lose its content.** Two rules, both
paid for:

- **`git commit --amend` composes a NEW tree from the INDEX, not from HEAD's
  tree.** So a `git checkout <ref> -- <path>` used as "cleanup" — which leaves
  the worktree and index mixed — is caught by the next amend, which commits
  the mixture. At R25 (2026-10-01) a nine-file fix became an EMPTY tree while
  its message still named all nine files: the trailer had been indented, the
  message was rewritten with `--amend -F`, and the index (not HEAD) became the
  new tree. Before amending a message, verify that `git diff HEAD` **and**
  `git diff --cached HEAD` are both empty, and `git reset --hard HEAD` first
  if either is not. Never `git checkout <ref> -- <path>` in a producer's
  worktree to "clean up" — `git reset --hard HEAD` keeps the worktree in
  lockstep with HEAD. If the destructive amend already happened, the reflog
  retains the pre-amend SHA: `git reset --hard <pre-amend-SHA>`, re-amend, then
  compare `git rev-parse HEAD^{tree}` with `<pre-amend-SHA>^{tree}` — they must
  be identical.
- **Amend in the worktree where the BRANCH is checked out.** `--amend` in a
  detached worktree does not move the branch, so the branch keeps the old
  subject. That matters most for a subject change: `fix #N` is a GitHub
  closing keyword and `narrow #N` is not, so a partial fix whose subject was
  never actually changed closes the whole issue when the gate pushes. Amend
  before the pick (the pick records the SHA; the push closes), and prove the
  amend was lossless with the patch-id, not with a tree diff — the SHA changes
  either way, so report both SHAs.

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

## OpenCode mechanics (this harness)

*Added 2026-11-03. This area exists because the harness's own agent-dispatch
surface behaves in a way that is not visible from the tool description, and a
coordinator that does not know it will spend whole turns waiting for agents that
are already running. Everything here was MEASURED on this box, not inferred.*

### ⚠ Dispatch a lane agent with `scripts/lane_dispatch.sh` — the harness `subagent` tool BLOCKS the turn

**⇒ THE RULE: EVERY lane dispatch goes through `scripts/lane_dispatch.sh`. NEVER
the harness `subagent` tool.**

**⇒ WHY — MEASURED 2026-11-03 (R38).**
- `subagent` with `background: true` returns a session id, but the HARNESS held
  the coordinator's **turn** open until the agent finished: the coordinator's
  next tool call did not run until the agent was done. The pool row read 7
  (it counts CLAIMS) while ONE agent was actually running, and the box was idle.
- Issuing several `subagent` calls in ONE message did **not** help: the harness
  ran the FIRST one in the foreground and blocked on it.
- `opencode api session.create` + a DETACHED `opencode run` returns in
  **0.57 s**, and the child session is LIVE immediately.

**⇒ THE RECIPE** — `scripts/lane_dispatch.sh --title <t> --log <path> --brief-file <path>`
(also accepts `-- <brief text>`), which is two steps:

1. `opencode api session.create --data '{"title":"…","parentID":"$OPENCODE_SESSION_ID"}'`
   — creates a CHILD session of **this** coordinator session.
   `OPENCODE_SESSION_ID` is exported in the environment.
   ⇒ **THE CHILD APPEARS IN THE UI UNDER THIS SESSION**, and
   `scripts/pool_lanes.sh` counts it exactly like a harness-spawned lane
   (it counts child sessions with no outcome yet).
2. `setsid nohup opencode run --session <id> --model minimax/MiniMax-M3.1-Flash
   --agent general "<brief>" >log 2>&1 &` — **DETACHED**, so the coordinator's
   turn continues immediately.

**⇒ THE MODEL AND THE AGENT MUST BE SET EXPLICITLY.** A session created through
the API and prompted with a bare body uses the server's DEFAULT agent and model
— measured `build` / `opencode` / `pulse-alpha-free`, which is **not** the
campaign's model. ⇒ `--model minimax/MiniMax-M3.1-Flash --agent general` is
part of the recipe, not an option. ⇒ The API's `session.prompt` **IGNORES** a
`model` field in the body (measured), so the model must come from the
`opencode run` command line.

**⇒ IF YOU MUST USE THE API DIRECTLY**, the body shape is `{"text": "…"}` — a
`parts` array is rejected with `Missing key at ["text"]` — and `session.prompt`
returns in **0.06 s** without waiting for the agent. ⇒ `session.prompt` alone is
non-blocking, but it cannot set the model; use it only as a follow-up to a
session that was already launched with the right model.

**⇒ THE SCRIPT PRINTS THE SESSION ID. RECORD IT IN `claims.tsv` IN THE SAME
TURN**, as `live:<role>:ses_…`. ⇒ A claim with no session id is a PHANTOM, and
`scripts/pool_lanes.sh` exits 3 on a ledger/opencode mismatch. ⇒ A lane is not
"dispatched" until its AGENT exists.

**⇒ NEVER `git stash`, NEVER `git add -A`, NEVER TWO `cmake --build` ON ONE
BUILD DIR** — and **⇒ NEVER LAUNCH A GATE WHILE A LANE IS BUILDING.**

### ⚠ The withdrawn rule: "put the CALL in the background, and put them ALL in one message"

*Withdrawn 2026-11-03, after it was MEASURED FALSE. It is kept here so a
coordinator who remembers it can see why it is wrong.*

**⇒ THE OLD TEXT WAS: EVERY lane dispatch is a `subagent` call with
`background: true`, and all the dispatches for a turn go out together as
parallel tool calls in a single assistant message.**

**⇒ WHY IT IS WITHDRAWN.** `background: true` marks the *agent* as asynchronous
but the harness held the *turn* open until the agent finished — so the rule's
own remedy (one message, several calls) ran the FIRST call in the foreground
and blocked on it. ⇒ The rule solved the symptom it was written for and left
the turn serialized, which is worse than useless when the pool must reach 7
lanes in a single turn. ⇒ **Use `scripts/lane_dispatch.sh`; the harness's
`subagent` tool is not a lane dispatcher in this harness.**

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
  ⚠ **Never declare a capability unavailable from a PACKAGE query — measure it.**
  `rpm -q libasan` says "not installed", and clang still ships its own
  compiler-rt ASan runtime, so `clang++ -fsanitize=address` links, runs and
  produces real findings. The general shape: a missing package is not a missing
  runtime, and a line of output is not a capability. **Read the COUNTER, not the
  parenthetical** — the harness prints
  `NATIVE SANITIZER: <counter> check(s) skipped (ASan/TSan runtime unavailable — …)`
  *whatever* the counter is, so the parenthetical carries no information at all
  (#7122, quoted from `tests/test_main.cpp:348-364`). Quote the counters, and
  plant a sentinel that must trip the sanitizer, so a zero leak count on a
  *disarmed* leg does not read like a clean one.
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

- **Lane worktrees go in `/mnt/fast/worktrees/<name>`**
  (`git worktree add /mnt/fast/worktrees/<name> <branch>`; the directory is
  shared, so name it like the branch). Never `.claude/worktrees` or anywhere else
  on `/home`, which rules out the Agent tool's `isolation: "worktree"`: put the
  absolute path in the brief instead. There are two reasons:
  - A lane build is create-dominated, and `/home` (the OS HDD) sustains ~5
    durable creates/s against the NVMe's ~846.
  - With the worktree and its `build/` on one filesystem, `build/` is a real
    in-tree dir and CMake turns on relocatable ccache keys (`base_dir` plus
    `-ffile-prefix-map`, #5010), so compiles are keyed by content, not by
    worktree path. Measured: a fresh worktree at an already-built commit got 483
    of 487 cacheable compiles from ccache (99.2%), and `bezel_tests_sema` built
    in 140 s instead of 1449 s. Across the 2026-09-29/30 runs, 38.7% of lane
    compiles duplicated a compile another lane had already done, which
    per-worktree keys could never share.

  Concurrent cold builds of the same sources still each compile; only finished
  compiles are shared. A worktree on `/home` still works (`lane_setup.sh` gives
  it the old `build/` symlink into `/mnt/fast/lane-builds/` and warns), but it
  shares nothing.
- **Moving a worktree onto the NVMe.** `git worktree move` cannot cross
  filesystems. Copy it with `rsync -aH` leaving out the build dir, run
  `git worktree repair <new path>` from the main checkout, then `lane_setup.sh`
  in the new place. Never carry a build dir across: its `CMakeCache.txt` binds
  the old source path. So the first build after a move is a full one, served
  from ccache when another worktree has built the same sources. On 2026-10-01 the
  worktrees active in the previous day were moved this way; older ones stayed in
  `.claude/worktrees/`.
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
  output, which looks exactly like a harness failure. Scratch goes in
  `/var/tmp/bezel-scratch/`, worktrees in `/mnt/fast/worktrees/`.
- Uncommitted WIP recovered from a crashed run is an **unverified hypothesis**,
  not work that is 80% done. Re-derive the diagnosis from the issue and HEAD:
  5 of 5 inherited WIPs were once defective, and two would have shipped a bug.
  Concretely, after a restart run `git status --short` **and read the diff** in
  EVERY lane worktree, not just the ones you remember: a crashed session can
  leave a fail-WITHOUT revert in the tree (the `git checkout -- <file>` for the
  proof, killed before the restore), which inverts the fix under review while
  the branch still looks correct.
- **Never wrap `git switch` or `git checkout` in a `timeout`.** A killed
  checkout leaves a HALF-switched tree — some paths updated, others not — and
  the damage reads as a crashed lane's WIP rather than as the damage it is.
  Let it finish, or check `git status` and repair by hand.
- **Never `checkout -f --detach` in another lane's worktree.** A reviewer's
  "reclaim" clobbered a live lane once; every worktree's reflog is the
  witness. Two reviewers must not share a worktree either: their fail-WITHOUT
  reverts interleave, and neither can say which one is in the tree.
- **Lock the warm lane pool.** `scripts/gc_scratch.sh` reaps every landed
  worktree after a push, and with it the in-tree `build/` and any reference
  compiler it holds. `git worktree lock --reason '<run> warm lane pool'` each
  lane worktree (and any worktree holding a reference build); gc honours
  locks — it skips every worktree the porcelain list marks `locked`. Unlock and
  `git worktree remove` explicitly at close-out.

### Reaping stale processes, worktrees and build dirs

The owner's standing permission (2026-11-05): **a coordinator may reap anything it judges
stale**, processes, worktrees, build dirs and scratch, using these scripts or its own. Only
**commits** and **live lanes** are protected. The automation runs only after a push, so a
long run that never pushes reaches the reaper only by hand. Reap **at every heartbeat
whose `df -h /mnt/fast` reads ≥ 80 %** (`run_status.sh` raises its disk ACTION only at 90 %, which is too late for a cold-build fan-out), and before every pool refill that will cold-build.

**Order: processes first, then worktrees, then scratch.** A worktree with a process in it is
not free, whatever its claim row says.

1. **Processes.** `scripts/reap_stale.sh` (dry-run), then `--kill`. Tier A covers ownerless
   processes, tier B aged ones, and tier 0 exempts anything still making CPU or I/O progress.
   `scripts/reap_orphans.sh` is the older PPID==1-only subset. Never kill, by hand or by
   script:
   - **a background waiter of a LIVE agent** (a scratch `oc_wait.sh`-style poll loop, a `while … sleep` loop, a
     `wait_pid.sh`). The agent is woken by that shell's exit; kill it and the agent never
     wakes, holding its slot until someone notices. Measured R39: four agents sat
     "completed, waiting" on runs that had already finished. Map each waiter to a live claim
     before signalling it;
   - the coordinator's own waiters (`ListAgents` plus the background-task list);
   - a running gate (`$BEZEL_GATE_PIDFILE`).
2. **Which worktree is free.** For each `/mnt/fast/worktrees/*`, check:
   - **procs**: the processes whose `/proc/PID/cwd` lies under it, which must be 0;
   - **claim**: the last `claims.tsv` row naming the path, which must not be `live`;
   - **dirty**: the tracked changes, from `git status --short | grep -v '^??'`.

   A tree with a live claim, or procs > 0, is never touched. Reusing a busy tree for a new
   lane is the same mistake: check procs before writing the path into a brief.
3. **Worktrees: reclaim the build or the whole tree.**
   - **Landed** (every commit is in `origin/main` or the staging ref by patch-id, not just
     by ancestry): `git worktree remove --force`. The branch ref stays.
   - **Unpushed commits, clean tree**: the commit is the asset, the checkout is not. Remove
     the worktree, and keep the branch. A **detached HEAD** has no branch: first
     `git branch keep/<wt>-<sha8> HEAD`, or the commit becomes unreachable.
   - **WIP (dirty tracked files)**: keep the tree and delete only `build/`
     (`rm -rf <wt>/build`; `scripts/lane_setup.sh` recreates it, and ccache makes the
     rebuild cheap). This reclaims ~3–4 GB per tree and loses nothing.
   - **A reference-compiler tree** a reviewer or verifier is reading
     (e.g. `r39-verify-shared`): lock it (`git worktree lock --reason …`) and leave it.

   `scripts/gc_scratch.sh --worktrees` automates the landed case only. It reports
   clean-but-unmerged trees and never reaps them; the cases above are done by hand, or by
   the reaper's newer modes once they land.
4. **Orphan build trees and scratch.** Run `scripts/gc_scratch.sh --builds --root`
   (`--apply`), which removes `/mnt/fast/lane-builds/*` with no worktree once it has been idle
   24 h, and quarantines old root scratch. A fresher orphan tree is removed by hand.
   **A waiter whose own `pgrep -f` matches itself** (target gone, loop eternal) is wedged,
   not live: killing it RELEASES the agent waiting on it. Confirm the target finished first. Delete per-lane scratch dirs (`/var/tmp/bezel-scratch/<run>/<lane>/`) by
   **name**, never with a recursive search over the scratch root (369 GB, see *Build
   toolchain*). Keep the lane's `report.md` and probes until its pick or rejection is
   recorded, because a harvest lane reads them.
5. **Record it.** Append `standdown` rows for the claims whose worktrees you removed, and
   note the before/after `df -h /mnt/fast` in the run cursor.

Measured 2026-11-05 (R39): **105 worktrees removed and 21 build dirs dropped took
`/mnt/fast` from 89 % to 55 %**, with no commit lost: every detached tip got a `keep/` ref
first, and every WIP tree kept its sources.

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

### ⚠ THE BUILD GATE — AND THE COLD-BUILD STORM THAT KILLED FOUR LANES AT ONCE

**MEASURED 2026-11-04 00:26 (the R38 run).** Four lanes were dispatched at once; **all four
cold-built at once**; the shared jobserver went to `available=0 held=20 builds=1 jobs=21`
with 22–44 builder processes observed box-wide; **and all four lanes DIED.** The signature
was the familiar one — `gmake … Terminated`, no `error:` lines — so each lane's death read
as "my build failed" and each had already lost its in-flight work.

⇒ **⇒ ⇒ ⇒ THE CAP ON *LANES* IS NOT THE WHOLE RULE — THE CAP ON *CONCURRENT COLD BUILDS*
IS. A LANE COUNT OF 5 STILL KILLS THE POOL IF ALL FIVE COLD-BUILD IN THE SAME WINDOW.**

⇒ **⇒ ⇒ ⇒ ⇒ THE BUILD GATE — EVERY LANE BRIEF MUST CARRY IT, VERBATIM:**

```
python3 scripts/build_jobserver.py status     # prints available=N held=N builds=N
```

**⇒ WAIT (poll every 60 s) UNTIL `available` IS AT LEAST 6, THEN BUILD. ⇒ BUILD WITH
`-j 4`, NOT `-j 20`.**

- **Poll the jobserver BEFORE the first build, not after.** A lane that starts a build
  without a gate does not know the pool is saturated.
- **`-j 4`, not `-j 20`.** One cold build at `-j 4` holds ~4 of the 20 tokens; at `-j 20`
  it takes the whole pool and the next lane's build gets nothing.
- **A lane that WAITS IS ALIVE; a lane that dies delivers NOTHING.** Waiting for the gate
  is the correct behaviour, not a stall to be worked around.
- **⇒ DISPATCH IN PAIRS, WITH A STAGGER.** Two lanes now, then the next two a few minutes
  later — so their cold builds cannot collide. Never dispatch the whole pool in one burst.
- **⇒ THE GATE BELONGS IN EVERY LANE BRIEF**, including a reviewer's: a reviewer that
  measures the RED arm needs a build too, and it is the reviewer's build that starves a
  sibling.

### ⚠ Pick a branch's commits IN THE ORDER THEY WERE AUTHORED — THE ANCESTOR FIRST

**MEASURED 2026-11-04 (the R38 run).** A branch carried three commits that all touch
`LlvmBackend_Calls.cpp`. The **later** commit's diff was contextualised on the **earlier**
one: cherry-picking the later commit first produced a diff that no longer applied, and the
branch had to be re-ordered.

⇒ **⇒ ⇒ ⇒ PICK A BRANCH'S COMMITS IN AUTHORED ORDER — THE ANCESTOR FIRST.** A later
commit's diff is written against the earlier commit's tree; picking it first asks the
cherry-pick to apply a patch whose context has not landed yet.

⇒ ⇒ ⇒ ⇒ This is not the same as "pick the whole branch at once" — the picks are individual
commits, and the *order* of those picks is the ancestor-first order.

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
heartbeat must never launch a gate and never push. Keep its interval coarse (tens of
minutes); a tight interval burns budget re-reading state that has not moved.

⚠ **BUT A HEARTBEAT THAT ONLY OBSERVES A STALLED POOL WATCHES THE STALL.** The heartbeat's
standing rule was amended on 2026-11-03 (owner directive, *"REKINDLE THE POOL"*) and
is now: **the heartbeat STATUSES and then REKINDLES — a partial pool is a REFILL, not a
report-and-stop.** Measured on the R38 run: the pool fell to 1 live lane of 7 and stayed
there, because the old prompt said *"report it and stop"* — so the heartbeat reported the
partial pool and stopped, twenty minutes later reported the same thing again, while the box
sat **completely idle** (0 lane build processes, 0 heavy processes box-wide) with six of
seven slots empty. It also PICKs landed commits whose non-author review has recorded a
verdict, so the banked count keeps rising between heartbeats. It still never launches a
gate, never pushes, never edits the staging worktree.

### ⚠ "DRAIN THE POOL" MEANS **LET IT FINISH** — NOT KILL IT

**Owner correction, 2026-11-04 01:06.** The phrase was read as "stop the agents", and the
coordinator **TERMed all five running lane agents** — mid-measurement, mid-build, with
uncommitted work in flight in four worktrees. The owner: *"drain the pool means let the pool
finish, and not dispatch more work, not kill them."*

⇒ **⇒ ⇒ ⇒ ⇒ "DRAIN THE POOL" = (a) STOP DISPATCHING NEW WORK, (b) LET THE RUNNING LANES
FINISH AND DELIVER THEIR COMMITS, (c) HARVEST THEM, and only then (d) go quiet for the gate.**

⇒ **⇒ ⇒ ⇒ ⇒ ⇒ A DRAINED POOL IS STILL A BUSY BOX.** A lane winding down is still compiling,
still running its test shards, still holding jobserver tokens. **⇒ ⇒ ⇒ ⇒ ⇒ ⇒ ⇒ THE GATE'S ZERO-
BUILDERS PRECONDITION IS *NOT* SATISFIED BY A DRAINED POOL** — the lanes must finish, and their
builds must actually stop, before `scripts/no_builders.sh` reads clean.

⇒ **⇒ ⇒ ⇒ ⇒ ⇒ ⇒ AND THE LANE PROCESSES ARE NOT TO BE KILLED TO MAKE THE GATE HAPPEN.** A lane
killed mid-turn loses its *turn*, not its *work* — the uncommitted changes stay on disk in its
worktree and the session keeps its history, so `scripts/lane_dispatch.sh --resume <session-id>`
picks it up exactly where it stopped. **⇒ ⇒ ⇒ ⇒ ⇒ ⇒ ⇒ THE RESUME IS CHEAP AND THE WORK IS NOT
LOST.** Killing is the LAST resort, and it is never needed to satisfy the gate: the gate wants
the lanes to STOP, and a finished lane stops by itself.

⇒ **⇒ ⇒ ⇒ ⇒ ⇒ ⇒ THE GATE STILL NEEDS QUIET — BUT THE QUIET COMES FROM THE LANES FINISHING, NOT
FROM THE COORDINATOR KILLING THEM.** ⇒ ⇒ ⇒ ⇒ ⇒ ⇒ ⇒ ⇒ ⇒ **AND THE MEASUREMENT IS THE EVIDENCE:
`scripts/no_builders.sh` AND `ps -eo args | grep -c "[o]pencode run"`, not an assumption.**

### ⚠ THE POOL TARGET IS **5**, NOT 7 — AND OVER-LAUNCHING THE POOL IS A REAL FAILURE MODE

**Owner directive, 2026-11-03 ~23:05: *"reduce the pool to 5 agents. record this durably.
now that you've been launching them effectively, we ran out temporarily. now it's good."***

**⇒ ⚠ ⇒ ⚠ SUPERSEDED IN PART BY THE OWNER DIRECTIVE OF 2026-11-04 ~13:55: *"Raise the pool to
7, review-weighted."* — SEE *THE POOL TARGET IS 7* BELOW. THE 5-LANE RULE IS NOT DEAD; IT IS
NARROWED TO THE RULE THAT ACTUALLY MATTERS (TWO CONCURRENT COLD BUILDS, STAGGERED DISPATCH).**

**⇒ MEASURED, THE SAME EVENING: the coordinator over-launched the R38 pool to SIXTEEN and then
SEVENTEEN live lanes (each a separate `opencode run` process with its OWN build tree on the
NVMe), the box ran out of build budget — `available=0 held=20`, dozens of builder processes,
disk at 80% — and then **ALL FIFTEEN LANES DIED AT ONCE**, in the same window, each leaving a
`live` claim in the ledger behind it.** ⇒ **THE POOL IS A *RESOURCE* BUDGET, NOT A PRODUCTIVITY
SCORE: every lane in the pool is a compiler and a build tree, and the number that matters is
how many can hold a token at once, not how many have a claim.**

**⇒ ⇒ ⇒ THE STANDING TARGET FOR THIS RUN IS 5 LANES** (`CLAUDE.md`'s *Pools* bullet,
`docs/ops/heartbeat.md`, the heartbeat's own prompt, and the campaign's coordinator block all
carry 5). **⇒ ⇒ ⇒ AND THE LEDGER IS THE SAFETY NET THAT MADE THE RECOVERY POSSIBLE:** the
claims file is append-only, so a lane that died leaves its `live` row behind and the next
heartbeat can see exactly which lanes to restart, which deliveries to harvest, and which
verdicts to pick — **⇒ A DEAD LANE COSTS A SLOT, NOT ITS WORK.**


**The heartbeat's contract and the VERBATIM recipe that recreates the schedule (there is no
`schedule_update` — a change means delete-then-create, back to back) live in
[`docs/ops/heartbeat.md`](docs/ops/heartbeat.md).** A heartbeat is a session artifact and
dies with the session, so that file is what makes the next run inherit the new behaviour.


### THE POOL TARGET IS **7**, REVIEW-WEIGHTED — (owner directive 2026-11-04 ~13:55)

**Owner directive, 2026-11-04 ~13:55, in answer to a measured report that the box was
running at ~20 % CPU: *"Raise the pool to 7, review-weighted."***

**⇒ THE 2026-11-03 "reduce the pool to 5" WAS OVER-CORRECTION, NOT A HARD CEILING.** What
actually failed that evening was the **BUILD BUDGET** — sixteen and seventeen lanes each
cold-building their own tree — not the lane count as such. The rule that survives is the
concurrent-cold-build cap, not the pool size.

**⇒ MEASURED, 2026-11-04 13:51, at a 5-lane pool:**

| resource | reading |
|---|---|
| CPU | us+sy ≈ **19–20 %** (~4 of 20 cores), idle ~50 %, **wa 32–41 %**, runnable 0–2, blocked 9–12 |
| load | 25.8 — inflated by D-state, not by runnable work |
| memory | **6 GB of 31 GB used** |
| disk | 63 %, 85 GB free |
| jobserver | **19–20 of 20 tokens FREE**, 0 builds |

**⇒ ⇒ ⇒ THE BOTTLENECK WAS THE REVIEW QUEUE, NOT PRODUCTION.** Eight commits were awaiting a
verdict against four reviewer lanes, and one verdict costs **1.5–3.5 h** (a cold build, then
the whole `codegen` area at 22,900 rows, the whole `sema` area at 9,500, and the lit track).
⇒ **⇒ ⇒ ⇒ AT 7 LANES, WEIGHT TOWARD REVIEW:** a producer only deepens the queue a reviewer
must drain, so a marginal lane should be READ-ONLY (a reviewer, or an issue-verifier).

**⇒ ⇒ ⇒ ⇒ AND A MARGINAL REVIEWER LANE NEEDS SOMETHING TO REVIEW — WHICH NO LONGER EXISTS.**
Measured 2026-11-04 13:5x: `git cherry origin/main` over EVERY branch in the repo returns
**no** `fix #` / `narrow #` commit outside `origin/main` — every R35 and R38 lane branch is
already pushed, by patch-id. ⇒ A marginal lane's work must therefore be **VERIFICATION**:
reproduce an OPEN issue on the shipping tree and return a **DISPOSITION** —
*still reproduces* (a fix candidate, with the file:line of the defect and the smallest
plausible fix site) / *already fixed* (a hand-close candidate, with the fixing commit found
by `git log -S`) / *the repro is wrong* (close as stale) / *the rule needs an owner ruling*
(a spec-question, with the two readings quoted).

**⇒ ⇒ ⇒ ⇒ ⇒ WHAT DID NOT CHANGE:** at most **two** lanes cold-build `--area codegen` at
once; every lane brief carries the build gate (`available` ≥ 6, then `-j 4`, never `-j 20`);
the coordinator dispatches in **pairs with a stagger**, never the whole pool in one burst; a
lane that waits is alive; a lane that dies delivers nothing.

### ⚠ `rg` IS NOT INSTALLED IN THIS ENVIRONMENT — AND A LANE WILL REACH FOR IT

**MEASURED 2026-11-04 14:46–14:52.** The #8061 fix-forward's reviewer lane
(`r38-c2-review-8061ff`) ran `rg -n --no-heading -g '*.cpp' -g '*.hpp'
'stdlib §[0-9.]+' src tests`, got **`rg: command not found`** three times, fell
back to `grep -rn` over the whole tree, and **timed out at 120 s** — twice —
losing the better part of its turn to a tool that does not exist. The other
six lanes had not hit it yet; any of them could.

**⇒ ⇒ ⇒ A `rg` SHIM NOW EXISTS AT `/home/stepan/.local/bin/rg`** — a shell
script over `git grep` (index-backed, so the same sweep runs in **0.15 s**)
with a `grep -r` fallback outside a work tree. It supports the subset lanes
use: `-n`, `--no-heading`, `--no-line-number`, `-i`, `-F`, `-l`, `-c`,
`--count-matches`, `--hidden`, `-e/--regexp`, `-g/--glob`, a pattern on
stdin, and **searching stdin** when no path is given (so
`rg … | rg …` pipelines behave). It is **not** ripgrep: no `-P`/PCRE, no
`--json`, no multiline.

**⇒ `/home/stepan/.local/bin` IS ON EVERY LANE'S `PATH`** (it is the third
entry), so the shim applies to lanes **already running** and to every lane
dispatched after it.

**⇒ ⇒ ⇒ ⇒ TWO RULES FOR LANE BRIEFS, THEN:**
1. **A lane brief may say `rg` — the shim makes it work.** But a brief that
   tells a lane to sweep the whole tree should ALSO name the fast form:
   **`git grep -n --no-heading -E 'PATTERN' -- '*.cpp' '*.hpp' src tests`**,
   and should say **raise the timeout on a big sweep** (`timeout: 600000`).
2. **⇒ ⇒ ⇒ ⇒ ⇒ ⇒ AND A BRIEF MUST NOT ASK A LANE TO RE-DERIVE A SWEEP THAT THE
   COMMIT UNDER REVIEW ALREADY MEASURED.** The #8061 brief asked for a
   spot-check; the lane's own report listed the 115 candidate sites and the
   reason for each of the 97 left alone. **⇒ THE REVIEWER'S JOB WAS MINUTES OF
   SPOT-CHECKING, AND IT SPENT ITS TURN REBUILDING THE SWEEP.** ⇒ Point a
   reviewer at the lane's report and at the commit's own `--numstat`/`--stat`,
   not at the tree.

**⇒ ⇒ ⇒ ⇒ ⇒ ⇒ AND IF A LANE IS SILENT FOR A WHILE, LOOK FOR A TOOL WALL
BEFORE YOU CALL IT STUCK.** A lane with no output for minutes is usually
mid-reasoning or mid-sweep — but a lane whose log shows `command not found`
or repeated `timeout` messages is blocked, not thinking, and it needs a
follow-up (deliver it with `scripts/lane_dispatch.sh --resume <session-id>`).

### Gate protocol

1. **Never launch a gate you expect to fail.** Run every targeted filter for
   every known regression on the candidate HEAD first; if any FAILs, fix forward
   and re-check before launching. A gate you know will fail is performative and
   creates push pressure that leads to weakened oracles.
   ⚠ **Fetch before you measure, and compare DATES, not only refs.**
   `origin/main` is a remote-tracking ref: it moves only when you fetch. A
   coordinator who has not fetched since the last push measures staging against
   a stale `origin/main`, sees staging ahead, and concludes the batch is level —
   while the remote has moved on since. The mandatory merge (the gate cannot
   fast-forward a behind tree) then lands late, after the batch was validated,
   and it can conflict with the fix it carries. So `git fetch origin` first, and
   when you ask whether you are level with main, compare the **commit dates**
   (`git log -1 --format='%ci'`) as well as the refs: a ref that looks current
   can be days older than the branch it is compared against.
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
   ⇒ ⇒ ⇒ ⇒ ⇒ **⇒ RUN THE SWEEP *AFTER EVERY PICK*, NOT ONLY BEFORE THE GATE.**
   **MEASURED 2026-11-04 15:35.** The R38 run picked four commits (#8061,
   #8059, #8044, #8051) over two heartbeats and **never ran the sweep**. The
   sweep, run on the tip afterwards, found **2 of 25 RED** — and one of the two
   was **not** the known stale-`build/bezelc` one: the **#7972** pick had
   added a test row that trips **`check_ambiguous_ran_false`** (a rejection
   claim pinned only by `.ran`, which `runAot` reports false for FOUR
   outcomes, not one). **⇒ ⇒ ⇒ ⇒ ⇒ THAT ROW WAS IN THE TREE FOR ~40 MINUTES
   AND WAS ONLY CAUGHT BY ACCIDENT.** ⇒ A ratchet trip is a **GATE BLOCKER**:
   the gate cannot launch while it is RED, so a trip discovered at the
   endgame costs the whole endgame. **⇒ ⇒ ⇒ ⇒ ⇒ ⇒ THE SWEEP IS BUILD-FREE
   AND COSTS SECONDS — RUN IT AFTER EVERY `pick_stage.sh` AND FIX THE TRIP
   BEFORE THE NEXT PICK.**
   ⇒ **⇒ AND THE FIX FOR AN AMBIGUOUS-CLAIM TRIP IS *NEVER* THE GRANDFATHER
   FILE.** `scripts/ambiguous_ran_false.grandfather` is a ratchet EXEMPTION:
   adding a row to it silences a check that the row itself provoked. **⇒ IF
   THE ROW CAN BE MADE SOUND, MAKE IT SOUND** — the repo's own leak-oracle
   test (`tests/support/aot_leak_detection_4638_test.cpp`) already carries the
   protection: a `diagnosticMessageContains(r, "LeakSanitizer fired")` pin
   beside the `!r.ran` claim, which tells a deliberate oracle abort from an
   unrelated crash.
   ⚠ **The checkers fail CLOSED, so a checker run from the wrong directory cannot
   report a clean pass.** Measured 2026-11-02: every one of the 28
   `scripts/check_*.py`, run from a directory that is not a checkout, either
   measured its own tree correctly (they resolve it from `__file__`, not from the
   cwd) or exited non-zero with a diagnostic — `check_src_citations` printing
   *no document headings found -- wrong root?*, `check_dangling_capture` and
   `check_internal_residual` exiting 2 for a missing compile database or
   `bezelc`, `check_commit_trailers` exiting 1 for an unlistable range. None
   printed "clean" while having measured nothing. So a wrong invocation looks
   like a FAILURE here, never like a pass — and `ratchet_sweep.sh` cds to its own
   repository before it runs anything.
   ⚠ **Adding `runAotChecked` rows legitimately raises `checked_calls` and turns
   `check_toolchain_skip_surface` RED.** That is a *denominator* rise, not a new
   silent-skip site. Before `--update --force`, verify what the ratchet's own
   diagnostic asks: the new rows carry no `wasSkipped()`/`find("SKIP")` guard, no
   early return, assert `r.ran` unconditionally, and `absorbing_sites` **strictly
   fell** — a flat count is not a fall, and `--update` refuses a rise in
   `checked_calls` unless `absorbing_sites` fell (`chk_m > chk_b and
   abs_m >= abs_b` is the refusal). Bank it **once at batch level, on the merged
   tree**, not per lane — several lanes adding checked rows would otherwise all
   collide on the single baseline file, and the counts are only correct on the
   tree that carries all of them.
   ⚠ **A `src/` COMMENT edit is not free.** Several ratchets read the source
   text, so a comment that changes a shape, a word or a count can move a site
   from one bucket to another — `check_src_citations.py` alone greps every `§`
   in `src/` and fails the cycle on one that resolves nowhere. "Comment-only"
   means token-identical, not byte-identical. After **any** `src/` edit, comment
   or not, re-run `scripts/ratchet_sweep.sh` before the gate.
   ⚠ **Re-run the sweep after EVERY pick, on the staging tip** — not once at
   the end, and not on the lane's own tree. A pick is not safe until the sweep
   is green on the tip it landed on; a sweep run in the lane's worktree is the
   wrong-tree measurement, and a sweep run before the last pick says nothing
   about the last pick. ⚠ **A ratchet's verdict is relative to a BASE SHA.** A
   count measured against a lane's base is not the count on the merged head, so
   re-measure against the merged tree — the toolchain-skip ratchet's own
   diagnostic says as much: *re-run --update on the tree that fixed these
   rows*.
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
while most of the per-merge time is serial cognition. Run a **firm N=7** agents
in flight (producers + reviewers) and refill a slot in the same turn it frees —
except during a gate, when it is zero. Keep lanes **file-disjoint**: no two on
the same hot dispatch function (`typeCall`, `lowerBinary`, the `tryLower*Call`
family), and at most one disposal/ownership/async/iterator lane at a time.
Bundle at most 2–3 closely-related issues per lane, one commit per issue.

**The pool size is NOT the build budget, and the two were confused once already.**
The measured limit is **concurrent cold `--area codegen` builds (2)**, not the
number of lanes — see *At most TWO lanes may cold-build `--area codegen` at
once* below, whose incident is three lanes, not six. So **N=7 means seven lanes
and a cold-build QUEUE**, not seven simultaneous cold builds: a coordinator
running 7 producers must stage their first builds, two at a time. Raising the
pool does not raise the cap, and the cap is what keeps a lane's build from
being killed by the slice — a kill with zero `error:` lines reads exactly like
a build failure and is not one.

**Counting the live pool.** `claims.tsv` is append-only and a claim's CURRENT
status is its LAST row; the `status` vocabulary is `live` (with an optional
role after a colon, e.g. `live:producing`), `queued`, `done` and `standdown`
(`CLAUDE.md`, *Claims*). So the live pool is countable, and
`scripts/run_status.sh --claims <path> --pool N` counts it and **flags a pool
below its target** — which is the check that catches a run leaving a slot
empty. It did not exist until 2026-11-02, when a pool ran at 4 for nineteen
minutes because a lane that stood down was not refilled in the same turn and
nothing could see it.

**Sonnet is the default; dispatch Opus UPFRONT** — never as a fallback after a
Sonnet attempt came back short — when the task spans ≥2 predicates across files,
needs nested closure/captured-cell reasoning, is a 3+-issue cluster with one root
cause, has a suspect root cause, follows a *refuted* partial fix, or is **any
gate-failure fix-forward** (always Opus). Judge difficulty, not topic. If a
Sonnet fix regresses more than it fixes, revert it and re-dispatch as Opus.

**EFFORT is a separate dial from the model, and it is set per dispatch.**
Medium for a mechanical lane whose brief carries the diagnosis; high for a lane
that must rule on a spec question, for every soundness review, and for every
gate-failure fix-forward. Effort cannot be changed on a lane that is already
running, so the dispatch is the only chance to set it. A refuted Opus lane gets
a **second Opus attempt** before the model is escalated: escalating on the first
refutation spends the strongest model on a hypothesis that was never tested.
Escalate the model only after two refutations, or when there is no root-cause
hypothesis to act on at all.

### ⚠ Dispatching a lane agent — see *OpenCode mechanics* (this section is superseded)

*This section used to carry the rule "every lane dispatch is `background: true`,
and all the dispatches for a turn go out together as parallel tool calls in a
single assistant message". **That rule was MEASURED FALSE and is withdrawn**
(2026-11-03): the harness's `subagent` tool holds the coordinator's turn open
even with `background: true`, and several `subagent` calls in one message run
the FIRST one in the foreground.*

**⇒ THE RULE NOW: dispatch every lane with `scripts/lane_dispatch.sh`, and
never with the harness `subagent` tool. The full recipe, the measured
numbers, and the API details are in this runbook's *OpenCode mechanics (this
harness)* area.**

**⇒ NEVER `git stash`, NEVER `git add -A`, NEVER TWO `cmake --build` ON ONE
BUILD DIR** — and **⇒ NEVER LAUNCH A GATE WHILE A LANE IS BUILDING.**

### Review and dispatch: where the contract rules came from

*Added 2026-10-04. Until then these rules lived only in coordinators' memory files and in
per-run briefs. `review-contract.md`, `peer-contract.md` and CLAUDE.md's *Pools* bullets state
them; this section keeps the incident behind each one, so a rule that looks fussy can be judged
by what it cost. Dates are from the coordinators' notes at the time.*

**The review is a separate agent, never the producer's own handback.** Cycle 25 (2026-09-25
02:35): a peer offered #4859 (`ceacb92b`) for pick on the strength of the producer's handback.
The integrator refused it: a handback describes the producer's own work, including its own
judgement that five divergences were "pre-existing". Those had to be re-measured at the base by
someone with no stake in them. That is why review-contract check 7 exists.

**A freed producer slot goes to its reviewer first.** Cycle 25: a producer handed back #4770
(`2dde041d`, a §30.6/§5.1 ordering change on async return), and its freed slot went to a new
producer instead of the waiting reviewer. A fix with no reviewer in flight is one step from
being picked unreviewed, and it was exactly the kind of change that needed one.

**FAIL-WITHOUT per row.** R25, 2026-10-01: a reviewer ACCEPTed #5217 with "4/4 gtest PASS" on
the fixed tree and "0 gtest FAIL" on the base. Rows that pass on the buggy base do not pin the
bug. The aggregate hid it, and the integrator rejected the review.

**An admitting twin is GREEN on BOTH arms, and a brief that says otherwise is
wrong.** R35, #7172: a fix shipped a raise row and its admitting twin — the
same shape with a legal value, one character apart (`"ab".Insert(999,"z")`
raises, `"ab".Insert(2,"z")` stays admitted). The brief to the producer said
*"the two new rows must be mechanically RED on the base arm"*. The reviewer
corrected it: the twin is green on the base and on the tip, because the base
compiler admitting the legal value is **correct behaviour**, not a bug. Only
the raise row is red on base. So: when a brief pairs a raise row with an
admitting twin, say so — *the raise row must be RED on base, the twin must be
GREEN on both arms* — and treat the twin's green-on-base as the positive
control it is, showing the shape is genuinely admitted rather than rejected
for an unrelated reason. A reviewer who is handed such a brief must not reject
the lane because the twin stayed green.

**Run the accept rows the base rejected.** Cycle 25: two rows of
`ctor_chain_self_reference_4791_test.cpp` (`9c00771a`, #4791) asserted that invalid programs are
accepted and run. Row 1 printed `hello/0` (a silent zero-fill) and row 5 crashed with SIGSEGV.
The base had rejected both. The gtest rows passed because they checked only that the program
was accepted.

**Deleted rows are a REJECT.** Cycle 25: #4770 (`bd4a3a1a`) deleted
`async_forgotten_await_777_test.cpp`, with 8 rows including 5 that ran accepted programs. The
reviewer flagged the coverage loss and still said ACCEPT. Deleting regression rows weakens the
oracle whatever the fix's merit, so a review that reports deleted rows is a REJECT.

**Check a probe against the base first.** Cycle 25, #4791: a reviewer's soundness probe found
a silent accept and would have rejected the fix mechanically. The reviewer checked the base, the
gap was already there (the same root as #4824), and the fix was correct. It became ACCEPT-HOLD
until its codegen half landed. A pre-existing gap is an issue to file, not a rejection.

**Read the diff of a UNION resolution.** R22: a lane resolved #5082's cherry-pick conflict and
called it a UNION (`268a6c4e`). The diff showed it had taken the incoming side and dropped
#5045's `ParameterSymbol` branch. The reviewer had run the tests and a grep, and neither shows a
dropped branch. R21 cycle 26 (#4973): `--theirs` on a shared test file silently drops the other
commit's rows.

**Grep the leak log even when the rows pass.** R25 gate C, run 1 (staging `3758eb31`): the new
#5251 module rows each leaked 16 B through an imported-class temporary (#5268). They passed
every lane and reviewer ASan check, because only the gate's sharded verdict turns a leak report
in a *passing* test into a failure. That cost one gate cycle (~27 min) with the 02:00 deadline
approaching.

**`corpus.py --strict` is not the strict check.** R26: the #4972 producer and its reviewer
both ran `corpus.py` lint/check and missed three UNBANKED rows. `corpus.py` exits 0 on an
unknown subcommand. The real check is `scripts/gate_rule_corpus_baseline.py --strict`.

**The five-source pre-dispatch check.** R23, 2026-09-28: #4924 was dispatched with a three-source
check. Its fix (`b2ee727d`) had landed 207 commits earlier citing §22.3, not the issue number,
and the issue was CLOSED. The lane exited with no SHA, the fourth no-op of that run. R25: the
a24-4957 lane HALTed because #4994's fix (`b8f7b434`, the out-var recipient slot in chain-call
codegen) had already fixed #4957's symptom. A grep on the mechanism (`out var recipient`,
`emitChainCall`) would have caught it; the `#4957` grep could not.

**Claims and worktree paths.** R23, 2026-09-29: two lanes in one pool were dispatched into the
same worktree, and the second switched the branch under the first mid-build. Earlier runs had
the same collision across pools. `claims.tsv` is the one record both pools check. R26
(#4984/#4985): a brief built its worktree path from the branch name, and the real worktree had
another name. The lane measured the mismatch and asked for authorisation, and the permission
classifier, which authorises by the literal path, denied the rebase. So the path is always copied from `git worktree list`.

**Reuse worktrees.** 2026-09-27: the NVMe was at 27% of its rated write endurance after 35.8 TB,
writing ~0.8 TB a day during runs. Moving five-plus old worktrees onto a new base caused
simultaneous cold builds of ~3.6 GB each. Each pool keeps a fixed set and reuses it, adding a
worktree only to fill a free slot.

**Corpus rows are banked centrally.** 2026-09-23: two lanes each banked their own row in
`tests/conformance/rules/baseline.tsv`. One lane's three commits then all conflicted on the same
hunk at pick, although the changes were independent. The integrator banks corpus rows, like the batch-level ratchets.

**Stop every process before handing back.** R26 (5272-ff2): a producer's follow-up left a
`setsid nohup` child running after its own shell exited. The child was reparented to init, so
nothing could kill it by session. The reviewer then launched its own sweep of the same half, so
two ran at once, and the kill was refused as not verifiably this session's. Hence `kill -0` on every PID before handback,
including detached children.

### Publishing a static release

*Added 2026-11-02. The release is the owner's side task (asked for in R34, cut in R37), and the
procedure lived in a scratch script where it was one reaping away from being lost.*

The repository publishes a **fully static musl `bezelc`** as a GitHub pre-release, so a user can
download one file and run it with nothing else installed. The build is
`scripts/release_static.sh --full`; the publishing glue is **`scripts/publish_release.sh`**:

```sh
scripts/publish_release.sh <worktree> <tag> <title> <body-file> [branch]
```

Run it **after the gate has pushed `origin/main`**, in the worktree that holds that commit, with a
clean tree: the script refuses a dirty tree and refuses to build from anything that is not
`origin/main`'s tip, because a release of an unpushed commit is a release nobody can reproduce. The
tag and the tarball name carry the 8-character short sha (`r37-db3bdf98`,
`bezelc-0.0.0-db3bdf98-linux-x86_64.tar.gz`), matching the R34/R35/R37 pre-releases.

Two `gh` facts, measured 2026-10-30 (gh 2.87.3), cost one run each and are now encoded in the
script's header: **this `gh` has no `--body-file`** (it is `-F` / `--notes-file`), and **`--target`
wants the full 40-character sha** — a short sha is rejected with *Release.target_commitish is
invalid* (422).

Publishing to a public repository is a coordinator decision, and whether to cut a release at all is
the owner's call.

### Merging into the conformance corpus

A conformance case is a **whole file with its id in its name**, so a merge of
two branches that both added cases is not a textual merge — it is an id-space
merge. R28 (2026-10-09) is the incident: staging and `origin/main` each added
a *different* program under the same ids `s14_2_0021`/`s14_2_0022`, the files
conflicted as `add/add`, and `manifest.tsv` **auto-merged** carrying both
registrations under one id. The corpus would have run one case and silently
dropped the other, with no error anywhere.

- **A conformance `.bzl` conflict is an ID COLLISION until proven textual.**
  Compare the two sides' `//! case:` headers before reaching for a union.
  Union is *impossible*: one file cannot carry two `//! case:` headers and two
  `module Conf;` blocks.
- **`--ours` / `--theirs` on a conformance case DELETES the other side's
  program.** The resolution looks clean — no conflict markers are left, and
  `git rm --cached` does not error — while one side's case is simply gone.
  Restore with `git show <ref>:<path> > <path>` and re-`git add`, then verify
  by grepping the `title:` header, not by the absence of conflict markers.
- **`manifest.tsv` auto-merging is NOT evidence the merge is sound.** It
  happily holds two rows under one id. After any merge run
  `corpus.py check`: it is byte-exact, so a manifest row whose case file was
  deleted by `--ours`, or a row duplicated under one id, shows as *stale*.
- **Renumber to a VERIFIED-FREE id, and check all three tracks**: the case
  file, `manifest.tsv` and `baseline.tsv`. A baseline row is a `pass` for both
  `expect: accept` and `expect: reject` — it records the observed verdict
  conforming, not the polarity.

**`manifest.tsv` is derived, and it is derived byte-exactly.** Every column
comes from the case's `//!` header: the `check` column is the header's
`diagnostic:`, `stdout:` or `output:`. `corpus.py check` compares the file
byte-for-byte against the regenerated text, and `corpus.py build` rewrites it
wholesale — so a hand-edited manifest is either RED or silently reverted by
the next build. Change the header, then build. And **read the diff**: a build
whose diff touches rows you did not author means another case changed, which
is worth knowing before you commit it.

**Bank a baseline row as an in-place edit, not a regeneration.**
`runner.py --update` rewrites `baseline.tsv` in section order, so re-running
it over the whole corpus re-sorts the file and turns a one-row change into a
large diff. Edit the row where it belongs.

**The re-bank lands LAST.** Close the batch, pick everything, rebuild
`build/bezelc` for the final tip, run
`scripts/gate_rule_corpus_baseline.py --strict` once on that tip, and bank
exactly the UNBANKED rows that run reports. Every pick before the gate can
create a new UNBANKED row, so the strict run's count is the authority — not
the list of rows you expected to bank. Re-banking earlier is not wrong, it is
just work that gets redone.

### Harvest review probes into the corpus

*Added 2026-10-21 (R34), at the owner's request, after the same lapse happened twice. Revised
2026-11-02: the R37 per-case self-check, the effort tier and the case size, after R37's H9 and
H10 were both rejected.*

Reviewers and producers write dozens of probe programs per fix: base/tip matrices,
over-rejection sweeps, looped ASan shapes. Left in a scratch directory, they guard nothing.
Turned into conformance cases, they run at every gate. The owner asked for this in R30
(2026-10-14).

- **Trigger.** On every ACCEPT or ACCEPT-W-NOTES pick, the integrator appends the pick's probe
  directories (the reviewer's harvest list and the producer's handback name them; both live under
  `/var/tmp/bezel-scratch/`) to `harvest.tsv`, beside the run's `claims.tsv`, in the same turn as
  the pick. A peer does the same when it hands an ACCEPT to the integrator. Once the queue holds
  4–6 picks, and always before the gate, dispatch a harvest lane: a producer taking a pool slot,
  with scope `tests/conformance/rules/cases/` plus the derived `manifest.tsv`.
- **The harvest list is a file, not a paragraph.** The reviewer writes a structured `harvest.tsv`
  row per probe — probe, kind, expected, §, quoted sentence, SHA — and the harvest lane globs
  those files for the picked SHAs. A prose list in the handback is a transcription step, and the
  R34 handbacks carried prose lists: between 05:41 and 15:20 on 2026-10-22, about 27 picks went
  by with nothing queued. Reviewers do NOT author corpus rows themselves: those rows would be
  unreviewed, and many writers to `baseline.tsv` and `manifest.tsv` collide.
- **Size and effort.** Dispatch a harvest lane at **Sonnet with effort HIGH**. A harvest lane
  ordinarily writes **60 cases or fewer**, and the coordinator may set a higher number for a lane
  when a review has turned up a genuinely under-tested area and the probes show it — say so in the
  brief. What the size guards against is a batch large enough that one bad helper class silently
  poisons dozens of cases: in H10, one bad helper class reached 32. Raise the number deliberately,
  never by default.
- **Verdicts come from the spec.** Each case quotes the sentence that decides it, with section and
  revision. Write the case from the document and triage any disagreement with the compiler as
  `tests/conformance/rules/README.md` *What to do with the divergences* says: class 1 (the
  compiler is behind) and class 3 (the document does not settle it) keep the case, citing an OPEN
  issue; class 2 (the probe itself was wrong) fixes the case. A disagreement is never dropped.
- **No duplicates.** One case per distinct shape × position. Grep `cases/` for the shape first.
  Run `corpus.py audit` on the base and on the branch: the branch adds no twin group the base
  lacks. `corpus.py lint` reports 0 problems. A matrix of 1000+ cells is not harvested as cases;
  open an issue proposing a generator instead.
- **Every case passes a per-case self-check before the lane commits.** The eight points are in
  [`templates/harvest-brief.md`](templates/harvest-brief.md): the `rule:` is a verbatim excerpt
  (grep it, record the line number); the verdict is hand-traced against the cited sentence and
  never copied from a compiler run; every helper is spec-legal on its own, so the case has no
  second error ground; no host-defined behaviour is asserted; the case is not vacuous; a RED row's
  issue is OPEN and describes that exact divergence; only stdlib names that exist are used; and a
  reject fires at the single intended site, with its accepting twin compiling. The lane records the
  checks per case in a `selfcheck.txt` under its scratch directory and quotes that path in the
  handback. Effort is not a substitute for the checks: R37's H9 and H10 were both REJECTED, H10
  with 45 of 116 cases bad, and both were dispatched at Sonnet with default effort.
- **Manifest and baseline.** The harvest commit carries its regenerated `manifest.tsv`
  (`corpus.py build`, as `AUTHORING.md` says), so `corpus.py check` passes on the branch. The
  manifest is derived and byte-exact, so **read the build's diff** — a diff touching rows you did
  not author means another case changed (*Merging into the conformance corpus*). Two
  harvest lanes are never in flight at once, so the derived file cannot conflict. Banking
  `baseline.tsv` rows for the new cases is the integrator's, after the pick (*Corpus rows are
  banked centrally*); the README's "bank it in the same commit" applies to a fix that closes a
  divergence, not to a harvest.
- **Review and pick it like any other commit:** a non-author review, then `pick_stage.sh --kind test`.

**Lapses.** R30 (2026-10-15): about 20 reviewed fixes were picked without one harvest lane, and the
owner had to ask. R34 (2026-10-21): 4 picks again went without one, and the owner asked again. In
both runs the rule lived only in a memory file, so this section and CLAUDE.md now state it. R37
(2026-10-29): the rule was now in the repo, and the harvests still failed — H9 and H10 were both
REJECTED, H10 with 45 of its 116 cases bad. The rule said what to harvest; it did not say how to
write a case, so the defects were authoring defects: an invented `rule:`, an expected output
copied from a rejected fix's tip, a spec-illegal shared fixture, a host-defined stdout order, a
vacuous shadowing case, a RED row citing the wrong issue. Hence the self-check, the effort tier
and the cap, and hence the brief's committed copy rather than a scratch one.

### Commits

End every commit message with exactly one trailer line and nothing else — no
`Signed-off-by`, no second trailer — naming the model that wrote the commit
(`scripts/check_commit_trailers.py` checks the form, not a fixed model):

```
Co-Authored-By: Claude <Model> <version> [(context)] <noreply@anthropic.com>
e.g. Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>
```

Cite the spec section **and** revision in the body (`§28.2, v0.303`); the gate
runs `scripts/check_src_citations.py` and every `§`-citation in `src/` must
resolve. `narrow #N` is the partial-fix convention — but a closing keyword in a
**sibling commit's body** still auto-closes the issue, so `git log --grep="#N"`
before assuming work is outstanding. An OPEN issue is not evidence the work is
undone, and the open count is not a metric.

