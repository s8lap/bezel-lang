# Oracle ratchets and burn-down — 2026-09

*Supporting record (not a numbered proposal). Written 2026-09-17 as step 1 of issue
[#4452](https://github.com/s8lap/bezel-lang/issues/4452) ("Ratchet first, fix second"): the
specification of one ratchet plus one burn-down for each of the five measured test-oracle holes that
issue groups as a single programme. Base: `main` @ `00fb0ea0`, docs at the current revision (spec
v0.306). It specifies scripts, harness changes and gate phases; it ships none of them, and it
changes no normative document.*

> ⚠ **NO BUILD WAS AVAILABLE IN THE ENVIRONMENT THIS WAS WRITTEN IN.** The compiler needs LLVM 21
> and only clang 18 is present (issue #4451), so the suite, `gate.sh`, `agent_check.sh` and every
> ratchet script named below were **not run**. Every claim about the implementation here is a
> **source read, cited by file and line at `00fb0ea0`**. Where a number is quoted it carries its
> origin: either an **issue** (with the date that issue measured it) or a **`grep` over this tree on
> 2026-09-17**, which is a read of the source text and is labelled as such. The two disagree in
> places — the tree has moved — and every disagreement is called out rather than smoothed over.
> Nothing below was verified by execution. Re-measure at implementation time.

---

## 0. Why one programme

#4452 makes the case and is not restated. Two facts from it are load-bearing for the design:
three of the five holes are **silent skips**, and all five make a test **pass for a reason other
than the one it was written for**.

The programme is five instances of one shape, so it gets one shape of answer.

### 0.1 The shared design

Every ratchet in this programme is a **build-free Python script** in the family this repo already
runs. The family and its contract were read, not assumed:

| Member | Baseline | Read at |
|---|---|---|
| `scripts/check_deprecated_jit_oracle.py` | `scripts/deprecated_jit_oracle.baseline` | whole file, 433 lines |
| `scripts/check_ambiguous_ran_false.py` | `scripts/ambiguous_ran_false.baseline` | docstring + `main()` |
| `scripts/check_localdecl_only_walkers.py` | `scripts/localdecl_only_walkers.baseline` + `.grandfather` | docstring, lines 60–100 |
| `scripts/check_imported_symbol_reads.py` | `scripts/imported_symbol_reads.baseline` + `.grandfather` | flags + baseline header |
| `scripts/check_vacuous_tests.py` | none (zero-tolerance) | flags at :277–:280 |
| `scripts/check_src_citations.py` | none (zero-tolerance) | flags at :79 |

**Naming convention, already followed by every member:** `scripts/check_<name>.py` with
`scripts/<name>.baseline` (and `scripts/<name>.grandfather` when a per-file ceiling exists). New
scripts follow it exactly.

**The script contract** (each of the five below implements all of it):

- `--self-test` — plants a known violation in a `tempfile.TemporaryDirectory()`, never in the tree,
  and asserts the detector reports it. Precedent: `check_deprecated_jit_oracle.py:264–360`.
- `--strict` — the gate mode. ⚠ **Without `--strict` the scripts in this family exit 0 even when
  they found something** (`check_deprecated_jit_oracle.py:429`, `return 1 if a.strict else 0`;
  same at `check_ambiguous_ran_false.py:1078`). A gate phase that forgets `--strict` is a phase
  that cannot fail. Every phase spec below passes it.
- `--update` — re-banks the baseline after a burn-down; **refuses to raise a count**
  (`check_deprecated_jit_oracle.py:387–408`), `--force` for a deliberate rise.
- `--list` — prints every remaining row, so a lane can pick work without reading the script.
- **Exact, not a ceiling**: a count *below* baseline fails as STALE HEADROOM
  (`check_deprecated_jit_oracle.py:243–247`). Unbanked headroom is indistinguishable from a rise
  somebody already spent.
- **A control count where a recipe can drift.** The JIT ratchet banks `bare_grep` beside
  `live_calls` precisely so a file moving into `tests/support/` is not misread as a new call site
  (`check_deprecated_jit_oracle.py:201–216`). Holes 1, 2 and 5 below each bank a control for the
  same reason.
- **Marker mechanisms for the two legitimate exceptions**, ported from #3590 as implemented in
  `check_localdecl_only_walkers.py:75–100`: `<NAME>-ONLY: <reason>` removes a site from the count
  (a site proven correct by context); `<NAME>-DEBT: #NNNN <reason>` does **not** remove it but
  justifies a rise above the per-file ceiling in `scripts/<name>.grandfather`, which only ever
  falls.

### 0.2 Where a source ratchet is not enough

Holes 1, 2 and 4 are **runtime** properties: "the tier was armed", "the leg was available", "the
verdict was a verdict". A build-free script cannot observe them. Each of those sections therefore
specifies **two** things:

1. a **source ratchet** over the *surface* the runtime hole can disarm — call-site counts of the
   ambiguous form — so the blast radius cannot grow while the fix is pending; and
2. a **runtime assertion** — a *loud skip*: a counted chokepoint whose tally is printed
   unconditionally, carried in the shard summary, summed across shards and made to fail the run.

The runtime half is not invented here. It is the **#3626 shape**, already in the tree and read
end to end:

| Step | File:line |
|---|---|
| one counted chokepoint | `tests/native_test_harness.hpp:264–269` (`sanitizerRuntimeMaster` wraps the uncounted body and increments on `""`) |
| the counter | `tests/native_test_harness.hpp:65–68` |
| printed unconditionally, so `0` is positive confirmation | `tests/test_main.cpp:317–319` |
| carried in the shard JSON | `tests/test_main.cpp:368–370` |
| summed across shards, fails the run on nonzero | `tests/run_parallel.sh:480–495` |
| its own regression test | `tests/support/native_sanitizer_unavailable_loud_test.cpp:1–45` |

Its sibling is the #3007 infra channel, which adds the other half — **failing the current test and
demoting the diagnostic to `Severity::Note` so `hasErrors()` stays false**
(`tests/support/aot_support.hpp:608–628` `noteInfraFailure`, `:2381` `markInfra`,
`:565–:578` the at-exit `INFRA: N failure(s)` line). #3007's own reasoning for why a harness trip
beats a `JitResult` field — a field is "~477 edits away and re-creates the very opt-in-per-site
shape that produced the bug" (`tests/support/aot_support.hpp:494–503`) — applies unchanged to
holes 1, 2 and 4.

### 0.3 Gate phases and their order

All five phases are **build-free**, so they sit in `scripts/gate_full_cycle.sh` with the existing
ratchet phases, *before* the suite runs (`run_until_trustworthy` at `:913`). Each is modelled on
the src-citation phase (`scripts/gate_full_cycle.sh:852–867`): **self-test first, fail-closed, then
the check, fail-closed**, both appending to one per-phase log.

Existing order, read at `00fb0ea0`:

```
:529  vacuous-assertion phase (#3138)
:561  imported-symbol phase (#3152)
:599  enumerable-grid phase (#3419)
:629  localdecl-only-walker phase (#3591)
:659  jit-oracle phase (#3439)
:695  ambiguous-ran-false phase (#3892)      <- hole 3 lives here already
:737  null-guard-vacuous phase (#3992)
:792  gate-selftest phase (#3415)
:852  src-citation phase (#4455)
:882  rule-corpus phase
:913  the suite (plain, then --full)
```

The four new phases go in **one block immediately after `:714`** (the end of the
ambiguous-ran-false phase), so the whole oracle-ratchet family reads as one thing:
`toolchain-skip`, `native-asan-leg`, `timeout-negatives`, `uninstrumented-emit`. Order among them
is free — each is a few seconds of Python — but keeping them adjacent to the phase they extend is
what makes the programme legible to the next reader of that file.

---

## 1. `runAotChecked`'s toolchain SKIP disarms the ASan/LSan tier (#3733)

### The hole

`runAotCheckedImpl` probes `clang --version` once per process and, on failure, returns
`ran=false` with one `Severity::Error` diagnostic reading `"<name>: SKIP — C toolchain (clang)
unavailable for the ASan link"` — `tests/support/aot_support.hpp:2645–2653`. Call sites read the
literal `SKIP` out of the message and return without asserting. On a box where the probe fails,
**the whole ASan/LSan tier reports a clean PASS having run no sanitizer at all.** The in-source
comment above the probe (`:2634–:2644`) records the exclusion deliberately and names this issue.

| Quantity | Value | Source |
|---|---|---|
| `runAotChecked(` call sites | 1,551 | #3733, measured 2026-09-03 |
| … same recipe today | **2,046** in **634** files | `grep -rn 'runAotChecked(' tests --include=*.cpp` on this tree, 2026-09-17 (lines == occurrences, checked both ways) |
| skip-absorbing lines (`wasSkipped` / `SKIP (not a failure)`) | 1,204 | #3733, 2026-09-03 |
| … same recipe today | **1,518** across **270** files | `grep -rn 'wasSkipped\|SKIP (not a failure)' tests --include=*.cpp`, 2026-09-17 |
| call sites reading the literal out of the message | 224 in 224 files | `tests/support/aot_support.hpp:2636–2637`, measured 2026-09-03 |

The surface grew by ~495 call sites in 14 days. That is the argument for ratcheting before fixing.

### What the ratchet asserts

**Source half — `scripts/check_toolchain_skip_surface.py`.** Scans `tests/**/*.cpp` (all areas;
`tests/support/` included, as `check_ambiguous_ran_false.py` includes it, for the same reason). It
banks three numbers, on the JIT ratchet's live/control pattern:

- `checked_calls` — occurrences of `\b(runAotChecked|runAotCheckedLenient|runAotCheckedDisposal)\s*\(`
  in `tests/**/*.cpp` outside `tests/support/`. The denominator: the surface the SKIP disarms.
- `absorbing_sites` — the violation count. A site is absorbing when, inside one `BEZEL_TEST` body,
  a variable bound from one of those calls reaches a guard that returns without asserting on the
  strength of the fixed literal `"SKIP"`. Two recognised shapes, both present in the tree:
  (a) a call to an in-file predicate whose own body tests **only** the literal `"SKIP"` — the
  classifier for this already exists and must be reused, not rewritten:
  `check_ambiguous_ran_false.py:347` `_text_checking_helpers`, whose confirmed-SKIP-only logic is
  what that script's 2026-09-06 re-measure turned on (issue #3895 comment, 2026-09-06); and
  (b) an inline `if (<var>.diagnostics ... .find("SKIP") != npos) return;`.
- `support_calls` — the **control**: the same call tokens counted *without* the `tests/support/`
  exclusion. Its job is exactly `bare_grep`'s in `check_deprecated_jit_oracle.py:201–216`:
  distinguish "a new absorbing call site" from "a file moved into or out of `tests/support/`".

Exempt, and why: a guard that asserts something *after* observing the SKIP (for example, one that
fails the test when `BEZEL_AOT_FORCE_SANITIZE` is set) is not absorbing — it is the fix. A call
site with no guard at all is not absorbing either; it asserts on a `ran=false` it did not cause,
which is hole 3's defect, counted by hole 3's ratchet, not here.

**Runtime half — the loud skip.** The source ratchet cannot see whether the probe failed. Add, at
the one chokepoint `tests/support/aot_support.hpp:2645–2653`:

1. a process counter `checkedToolchainSkips()` incremented at that single return, in the shape of
   `tests/native_test_harness.hpp:65–68`;
2. an unconditional line from `tests/test_main.cpp`, beside the existing one at `:317–:319`:
   `CHECKED TOOLCHAIN: N skip(s) (clang unavailable for the ASan link — see
   tests/support/aot_support.hpp)`, so `0` is positive confirmation the tier was armed;
3. the same integer as an additive field `checked_toolchain_skips` in the shard JSON, at
   `tests/test_main.cpp:368–370` (additive keys need no schema bump — the comment there says so
   for `native_sanitizer_unavailable_skips`);
4. a sum in `tests/run_parallel.sh`, immediately after the block at `:480–:495`, failing the
   sharded run when the total is nonzero, with the same `GATE FAILURE:` banner shape.

That is #3733's own option 1 ("count and report it"), with option 2 ("make it fatal") reached by
the baseline hitting zero rather than by a flag day across 2,046 call sites.

### What the baseline file holds

`scripts/toolchain_skip_surface.baseline`, in the `key = value` shape of
`scripts/deprecated_jit_oracle.baseline` (whole-tree totals, not per-file — there is no per-file
triage to do here):

```
absorbing_sites = <N>      # to be re-measured at implementation time — the tree has moved
checked_calls   = <N>      # seed: 2046 (grep, 2026-09-17; #3733 measured 1551 on 2026-09-03)
support_calls   = <N>      # the control
```

Exact, not a ceiling, for all three: a drop that is not banked fails as STALE HEADROOM.
`absorbing_sites` may only fall. `checked_calls` **may legitimately rise** — new checked tests are
good — so it is banked as a control with `--update` allowed to raise it *only* when
`absorbing_sites` did not rise, and the diagnostic message must say which of the two moved, in the
shape of `check_deprecated_jit_oracle.py:201–216`.

### What the self-test plants

In a `TemporaryDirectory()`: a synthetic `tests/codegen/a_test.cpp` with one `runAotChecked` row
guarded by a local `wasSkipped`-shaped predicate that tests only `"SKIP"` (must count as
absorbing); a second with the same call guarded by a predicate that also tests real rejection text
(must **not** count); a `tests/support/x_test.cpp` copy (must move `support_calls` and not
`checked_calls`); then, in order, the four assertions the family's self-tests all make — matching
tree passes, planted violation fails, `--update` refuses the rise, an unbanked drop fails as STALE
HEADROOM, missing baseline exits 2. Model: `check_deprecated_jit_oracle.py:264–360`.

### How the gate phase invokes it

Phase name **`toolchain-skip phase`**, inserted at `scripts/gate_full_cycle.sh:715` (immediately
after the ambiguous-ran-false phase ends at `:714`). Log: `$LOGDIR/${TAG}_toolchainskip.log`.

```
say "toolchain-skip phase: detector self-test + surface ratchet (#3733)"
  self-test  -> CYCLE FAILURE: check_toolchain_skip_surface SELF-TEST failed — the
                detector is broken, so its verdict on the tree means nothing.
  --strict   -> CYCLE FAILURE: the runAotChecked toolchain-SKIP surface GREW — a new
                call site absorbs "SKIP — C toolchain (clang) unavailable" without
                asserting, so a box without clang runs that row's ASan tier vacuously
                (#3733).
  green      -> say "toolchain-skip phase: GREEN (...)"
```

The runtime half needs no phase: it fails inside `tests/run_parallel.sh`, which `scripts/gate.sh`
already consumes.

### Burn-down

Row classes and their replacement, in order of size:

1. **Guard removal (the majority).** Once the runtime half lands, a box without `clang` fails the
   run loudly, so the per-site guard has nothing left to protect against: delete the guard and let
   the row assert. Each deletion drops `absorbing_sites` by one.
2. **Guards in files that genuinely may run on a toolchain-less box** — if any exist. None was
   found by reading; see Open question Q1.
3. A lane banks progress with `python3 scripts/check_toolchain_skip_surface.py --update`, which
   refuses a rise.

**Done criterion:** `absorbing_sites = 0`, at which point the SKIP branch at
`aot_support.hpp:2648–2653` is replaced by a `noteInfraFailure`-style trip (#3733 option 2) and the
script is **kept as a zero-guard**, not retired — the shape is one grep away from returning, and a
zero baseline costs a few seconds per cycle. (Contrast hole 4, below, where the harness change makes
the shape structurally impossible and the guard is genuinely redundant.)

### Falsification

- If a box that fails `clang --version` nonetheless fails the suite for other reasons *before* any
  `runAotChecked` row runs, the tier is not silently disarmed and the ratchet is measuring a
  surface that cannot be exploited. #3733 argues the opposite ("the AOT harness cannot function at
  all without clang"), which if true means the SKIP is dead weight and the whole tier is *already*
  red on such a box — that would make the runtime tally permanently 0 and the burn-down trivial.
  **Either way the tally settles it, and the tally is the cheap experiment.** Run it first.
- If `absorbing_sites` turns out to be dominated by rows whose guard also protects a *different*
  skip (a `SKIP` emitted by something other than the toolchain probe), the predicate is over-broad
  and must key on the full literal `"SKIP — C toolchain"`.

---

## 2. The native per-file ASan leg is vacuous in lane worktrees (#3682, adjacent #3676)

### The hole

The per-file native ASan leg lowers a program with `emitObjectFile`, links it against an
ASan-instrumented runtime object, runs it, and classifies the result
(`tests/support/run_under_asan.hpp:88–110`). Its first act is
`const std::string& rtObj = ::bezel::native_test::asanRuntimeObject(); if (rtObj.empty()) return -1;`
— at `:129–:130`, `:162–:163` and `:196–:197`, once per family. `asanRuntimeObject()` is
`sanitizerRuntimeMaster("address")` (`tests/native_test_harness.hpp:288–290`), which returns `""`
when the runtime object cannot be built (`:183–:255`, six early returns, all memoised). In a lane
worktree it returned `""` (#3682, R12 gate 1, 2026-09-03), every helper returned `-1`, and every
caller's `if (... == -1) return;` made the leg pass **vacuously** — which is how #3653's
`detect_leaks` regression reached the gate with both the lane and its reviewer having "run the
converted suites with `BEZEL_AOT_FORCE_SANITIZE=address`".

| Quantity | Value | Source |
|---|---|---|
| files using a `runUnderAsan*` helper | ~40 converted by #3653 | #3682, 2026-09-02 |
| … today | **40** files | `grep -rlE 'runUnderAsan(Value\|ExitCode)?\(\|runUnderASan' tests --include=*.cpp`, 2026-09-17 |
| files touching `asanRuntimeObject()` directly | ~122 through the chokepoint (#3626) | #3682 / `tests/support/native_sanitizer_unavailable_loud_test.cpp:16–18` |
| … today | **80** files (`asanRuntimeObject()`), **13** (`buildRuntimeSanitizerObject`) | grep, 2026-09-17 |
| negative-return-swallowing guards near an ASan variable | **165** lines | `grep -rnE 'if \([A-Za-z_][A-Za-z0-9_]* *(<\|==) *(0\|-1)\) *return;'` filtered to lines naming `asan`/`san`, 2026-09-17 |

The gate's own floor already exists for this counter (`tests/run_parallel.sh:480–495`), which is
why the gate saw the regression. **A lane never runs `run_parallel.sh`** — CLAUDE.md and
`docs/ops/lane-contract.md:14–15` forbid it — so the floor is structurally unreachable from the place
where the vacuity happened.

**#3676 is live and has partly moved.** The helper at
`tests/codegen/domain_spawn_choice_valuearg_3181_test.cpp:153` still returns `-1` on link failure
(`:185–:189`), masking a real ASan link failure as a toolchain SKIP. But its caller is **no longer**
the `if (san < 0) return;` the issue quotes at its line 193: today it reads
`if (san != -1) BEZEL_EXPECT_EQ(san, 0);` at `:203`, so `-3`/`-4` now do fail. The residual defect
is exactly the stale `-1`, one line.

### What the ratchet asserts

**Source half — `scripts/check_native_asan_leg_skips.py`.** Scans `tests/**/*.cpp`. Two banked
counts, per file:

- `swallowing_guards` — the violation. A site is a swallowing guard when a variable bound from an
  ASan-leg call (`runUnderAsan`, `runUnderAsanValue`, `runUnderAsanExitCode`, any identifier
  matching `runUnderASan\w*`, or a direct `asanRuntimeObject()` / `buildRuntimeSanitizerObject()`
  result) is tested against a negative literal or `< 0` and the guarded statement **returns without
  asserting anything**. Both spellings count: `if (x == -1) return;` and `if (x < 0) return;`.
- `conflating_helpers` — the #3676 shape: a `return -1;` textually inside a function body that also
  contains a `-fsanitize=address` link command. `-1` means *toolchain unavailable*
  (`tests/support/run_under_asan.hpp:9–13`); a link failure is `-3`. One row per offending
  function. Seed: **1**, `domain_spawn_choice_valuearg_3181_test.cpp:188` (read 2026-09-17).

Exempt, and why: `if (x < 0) { BEZEL_EXPECT_... }` and the `if (san != -1) BEZEL_EXPECT_EQ(san, 0);`
shape at `:203` are **not** swallowing — they assert on the non-skip codes. A guard inside
`tests/support/run_under_asan.hpp` itself is the shared implementation and is exempt by path.

**Runtime half — the harness must refuse, not skip.** This is #3682's explicit ask ("a counter is
not loud enough when the test's own `if (asan == -1) return;` swallows it"). At
`tests/native_test_harness.hpp:264–269` — the one counted forwarder — when the result is empty
**and** `bezel::test::forceSanitizeAddress()` is true (`tests/support/force_sanitize.hpp:29–38`),
fail the current test the way `noteInfraFailure` does (`tests/support/aot_support.hpp:608–628`:
mark the current test failed, print a marker, count) instead of returning `""` for the caller to
swallow. Rationale, in the project's own words: the alternative is "~477 edits away and re-creates
the very opt-in-per-site shape that produced the bug"
(`tests/support/aot_support.hpp:494–503`).

Why gate it on `forceSanitizeAddress()` rather than always: a plain developer run on a box with no
ASan runtime should still be able to run the non-ASan rows, and `tests/native_test_harness.hpp:60–64`
records that the counter deliberately does **not** change `test_main.cpp`'s own exit code. Under
`--full`, and under `agent_check.sh --probe --asan`, the sanitizer is the point, so an unavailable
leg is a failed run, not a skipped one.

### What the baseline file holds

`scripts/native_asan_leg_skips.baseline`, per-file rows in the format
`scripts/localdecl_only_walkers.baseline:18` uses — count, whitespace, path:

```
# one row per file: swallowing guards (see the script docstring)
12   tests/module/xmod_owned_return_autodrop_1415_test.cpp
...
[conflating_helpers]
1    tests/codegen/domain_spawn_choice_valuearg_3181_test.cpp
```

Seeds: `swallowing_guards` ≈ **165** lines by the 2026-09-17 grep above (an upper bound: that regex
is line-based and does not bind the variable to its call, which the script must do) and
`conflating_helpers` = **1**. Both **to be re-measured at implementation time — the tree has
moved**. Exact, not a ceiling, both directions, per file.

Per-file rows (rather than one total) because this hole has **per-site triage**: some of the 165
may be rows that legitimately run on a box with no sanitizer runtime. Those get the
`// ASAN-LEG-ONLY: <reason>` marker (the `LOCALDECL-ONLY` mechanism,
`check_localdecl_only_walkers.py:75–100`), which removes the site from the count reviewably rather
than silently, plus `scripts/native_asan_leg_skips.grandfather` so a rise past the launch ceiling
needs a per-site `// ASAN-LEG-DEBT: #NNNN <reason>`.

### What the self-test plants

In a temp tree: a file with `int a = runUnderAsan(src, "t"); if (a == -1) return; BEZEL_EXPECT_EQ(a, 0);`
(counts); the same with `if (a != -1) BEZEL_EXPECT_EQ(a, 0);` (does not count — the `:203` shape);
one with `if (a < 0) { BEZEL_EXPECT_TRUE(false); }` (does not count); a `conflating_helpers` fixture
whose body contains `-fsanitize=address` and `return -1;` (counts) and one that returns `-3`
(does not); one `// ASAN-LEG-ONLY:` marked site (does not count); plus the five family assertions
(match passes, rise fails, `--update` refuses, unbanked drop fails STALE, missing baseline exits 2).

### How the gate phase invokes it

Phase name **`native-asan-leg phase`**, immediately after the toolchain-skip phase. Log:
`$LOGDIR/${TAG}_asanleg.log`.

```
say "native-asan-leg phase: detector self-test + skip-swallow ratchet (#3682)"
  self-test  -> CYCLE FAILURE: check_native_asan_leg_skips SELF-TEST failed — the
                detector is broken, so its verdict on the tree means nothing.
  --strict   -> CYCLE FAILURE: a new site swallows the native ASan leg's "toolchain
                unavailable" return without asserting — an ASan proof that is vacuous
                wherever the leg is unavailable (#3682), or a link failure is still
                reported as -1 (#3676).
  green      -> say "native-asan-leg phase: GREEN (...)"
```

### Burn-down

1. **`conflating_helpers` → 0 first**: one line, `return -1;` → `return -3;` at
   `domain_spawn_choice_valuearg_3181_test.cpp:188`. That is #3676 closed.
2. **Root-cause the emptiness** (#3682 ask 1) before touching the 165 guards; the candidates the
   issue lists are all path-resolution in `runtimeSourceDir()`
   (`tests/native_test_harness.hpp:98–113`) and the rt-objs cache key
   (`:196–:203`, keyed on `hash(srcDir)` — **a per-worktree key by construction**, which is the
   first thing to check).
3. **Then delete guards**, in batches per file, banking each with `--update`. With the runtime
   refusal in place a guard can only fire on a plain run where the leg is legitimately absent, so
   the correct replacement for most rows is no guard at all.
4. Rows that must keep a guard get `// ASAN-LEG-ONLY: <reason>` and leave the count.

**Done criterion:** `swallowing_guards` reaches its marker-only residue (every remaining site
carries an `ASAN-LEG-ONLY` marker) and `conflating_helpers = 0`. **Keep the script as a
zero-guard**: the `-1`-means-two-things shape is a copy-paste away from returning, and #3653
proved copy-paste is exactly how it spreads.

### Falsification

- If the runtime refusal fires on a box where the leg is *legitimately* unavailable and that box is
  one the project intends to support, the policy is wrong, not the code — and that is a decision to
  record, not to patch around (CLAUDE.md rule 4).
- If root-causing shows `asanRuntimeObject()` is empty in lanes for a reason the counter already
  reports, then the leg was never silent and the ratchet is redundant; the evidence against that is
  #3682's direct observation that the gate printed `NATIVE SANITIZER: 0 check(s) skipped` while the
  lanes were vacuous.
- If the 165 grep hits are mostly *not* bound to an ASan-leg call, the seed is inflated and the
  script's variable-binding pass (which the grep lacks) will say so on its first run.

---

## 3. `!ran` / `hasErrors()` cannot tell a rejection from a crash (#3892, #3895)

**This hole already has its ratchet.** `scripts/check_ambiguous_ran_false.py` (1,082 lines) +
`scripts/ambiguous_ran_false.baseline` + the gate phase at `scripts/gate_full_cycle.sh:695–714`.
This section is therefore *what is missing to make it complete*, not a redesign. It is also the
template the other four follow.

### The hole

`JitResult::ran` (`tests/support/codegen_support.hpp:53–63`) is false for four outcomes, all
reported as `Severity::Error` at `SourceLocation{}`: a genuine `Rejected`; `Crashed`
(`tests/support/aot_support.hpp:1015`); a **runtime** signal after a clean compile (`:2521`); and a
timeout or forced-ASan report (`:1001`, `:2495`, `:2512`). The enum is at `:630`. A row asserting
`BEZEL_EXPECT_FALSE(r.ran)` passes against all four.

| Quantity | Value | Source |
|---|---|---|
| bare-`!ran`-shaped rows | 248, of which 60 unprotected across ~35 files | #3892, hand-traced, 2026-09-05 |
| the script's own count at launch | 69 | #3895, 2026-09-06 |
| corrected (SKIP-only helpers are not protection) | 85 | #3895 comment, 2026-09-06 |
| corrected again (12 false positives) | 73 | #3895 comment, 2026-09-06 |
| **banked today** | **30** | `scripts/ambiguous_ran_false.baseline:16` (`count = 30`), read 2026-09-17 |
|  lines naming a shared `failedAtCompileTime*` helper today | **737** lines | `grep -rn 'failedAtCompileTime' tests --include=*.cpp`, 2026-09-17 |

The sweep is working: 73 → 30 while mentions of the shared `failedAtCompileTime*` helpers reached 737 lines (that grep counts every mention, including the header's own two definitions and its comments, so it is an upper bound on call sites). What follows is the tail.

### What the ratchet asserts

Already implemented; the predicate is stated in the script's own docstring
(`check_ambiguous_ran_false.py:25–66`) and is **the model for every predicate in this document**: a
row is `(file, BEZEL_TEST body, variable)`; the variable is bound from a rejection-observing
harness call (`BIND` at `:101–:106`); the body makes a rejection claim (`!ran` or `hasErrors()`);
and the body contains no protection of four recognised kinds — a direct `.message.find(`, a call
naming a marker helper (`NAME_MARKERS` at `:117–:120`), an in-file helper classified by its own
body, or a `.line`/`.column` nonzero check.

**Five completion items**, each a defect in the detector or a gap in its mechanism:

| # | Gap | Where | Fix |
|---|---|---|---|
| C1 | No suppression marker, so the deliberate demonstration row `tests/support/aot_infra_loud_3007_test.cpp::legacy_negative_assertion_shape` is counted forever and the baseline can never reach 0 | stated as a deliberate choice in `check_ambiguous_ran_false.py:61–70`; #3895 repeats it | add `// AMBIGUOUS-RAN-FALSE-OK: <reason>`, the `LOCALDECL-ONLY` mechanism (`check_localdecl_only_walkers.py:75–100`) |
| C2 | No per-file ceiling, so a rise needs only `--force` | `check_ambiguous_ran_false.py:1041–1062` | add `scripts/ambiguous_ran_false.grandfather` + `// AMBIGUOUS-RAN-FALSE-DEBT: #NNNN`, per #3590 as implemented in the localdecl ratchet |
| C3 | `_is_digit_separator` accepts a malformed digit-leading run (`9x'y'`), desyncing quote parity and hiding a trailing comment | `check_ambiguous_ran_false.py:177`; issue #3935, 2026-09-06 | validate the run against a partial pp-number grammar, as #3935 suggests; low priority — unreachable in compiling C++ |
| C4 | The scanner does not understand preprocessor conditionals, so text inside `#if 0` is scanned as live | #3935 names this as pre-existing and orthogonal | either skip `#if 0`/`#if false` blocks or state the limitation in the docstring; do not silently leave it |
| C5 | Helper classification reads only the file under scan, so a protective helper defined in a header is invisible | `check_ambiguous_ran_false.py:347` `_text_checking_helpers` + docstring `:68–:75` | also parse `tests/support/*.hpp`, which is where `aot_result_asserts.hpp` now lives |

C1 is the one that blocks the done criterion. The others are correctness of the count.

### What the baseline file holds

Unchanged in format: `scripts/ambiguous_ran_false.baseline`, a single `count = N` with a comment
header explaining exact-not-ceiling (`:1–:14`). Today `count = 30`. C2 adds a sibling
`.grandfather` with per-file ceilings; the baseline itself stays a total.

### What the self-test plants

Already implemented (`check_ambiguous_ran_false.py:572–1011`), including the rise, the STALE
HEADROOM drop and the missing-baseline exit. **Add one fixture per completion item**: a marked
`AMBIGUOUS-RAN-FALSE-OK` row that must *not* count (C1); a rise above a ceiling without a DEBT
marker that must fail even with `--force` (C2); the literal `int z = 9x'y';` line from #3935 with a
trailing `// ... BEZEL_EXPECT_FALSE(r.ran)` comment that must stay stripped (C3); a row inside
`#if 0` (C4); a row protected only by a helper defined in a fixture `tests/support/*.hpp` (C5).

### How the gate phase invokes it

Already wired: `scripts/gate_full_cycle.sh:695–714`, self-test then `--strict`, log
`$LOGDIR/${TAG}_ranfalse.log`, CYCLE FAILURE text at `:706–:709`. **No change needed** beyond the
script.

### Burn-down

The replacement per row class is settled and shipped
(`tests/support/aot_result_asserts.hpp:98–154`):

| Row class | Replacement | Why it discriminates |
|---|---|---|
| the row names a specific rejection diagnostic | `failedAtCompileTimeNotRuntime(r)` (`:113–:119`) | requires positive `"IR verification failed"` text and excludes `"terminated by signal"` / `"compiler CRASHED"` |
| the row has no text worth pinning (most) | `failedAtCompileTimeWithRealLocation(r)` (`:138–:154`) | a real `:<line>:<col>: ` prefix only ever comes from the driver's captured stderr, **and** it explicitly excludes the crash/signal/timeout/ASan texts |
| the row pins its own substring | a local `diagnosticMessageContains(r, "<text>")` (`:98–:102`) | any positive substring of a real rejection excludes all four synthetic texts |

⚠ **A generic `EXPECT_REJECTED(r)` macro is not proposed here.** The two helpers above are already
the distinguishing predicates, they are already shared and ADL-safe, and the header's own comment
(`tests/support/aot_result_asserts.hpp:26–58`) documents at length, with live-compiler evidence,
how a same-named shared function collides with per-file copies through ADL. A new macro would add
a third name for a decided question.

A lane banks a batch with `python3 scripts/check_ambiguous_ran_false.py --update` (which refuses a
rise) and lands in batches, never one giant PR (#3895's explicit instruction).

**Done criterion:** `count = 0` with C1 in place, the demonstration row carrying its
`AMBIGUOUS-RAN-FALSE-OK` marker. **Keep the script as a zero-guard.** The bare shape is the path of
least resistance for anyone writing a reject row, and #3892's evidence is that the correct idiom
"went from nonexistent to duplicated within one batch" (#3892 comment, 2026-09-06) — the pressure
is toward the wrong shape, continuously.

### Falsification

- If `--list` at count 30 turns out to be dominated by rows where `failedAtCompileTimeWithRealLocation`
  is *not* applicable (a rejection whose diagnostic genuinely carries no location), the remedy set
  is incomplete and a third helper is needed before the sweep can finish.
- If C5 (header-defined helpers) turns out to protect many currently-counted rows, the count is
  overstated and fixing the detector, not the rows, is the cheaper path — and the baseline must
  then drop via `--update` in the same commit that fixes the detector, or the gate reports STALE
  HEADROOM.

---

## 4. A compile TIMEOUT satisfies negative assertions (#3732)

### The hole

`forkedDriverStep` returns `{CompileOutcome::TimedOut, "runAot: compile did not finish within
<N>s; killed (compile hang / runaway front-end or link)" ...}` at
`tests/support/aot_support.hpp:1001–1006`; the run-side sibling is `"runAot: program did not
terminate within ..."` at `:2495`. `TimedOut` is in the enum at `:630` but **not** in the #3007
infra channel, so it surfaces as one ordinary `Severity::Error` — indistinguishable from a
rejection. The deadline is `aotCompileTimeoutSec()` (`:190`), 120s.

| Quantity | Value | Source |
|---|---|---|
| `BEZEL_EXPECT_TRUE(<var>.diagnostics.hasErrors())` sites reaching this harness | 272 | `tests/support/aot_support.hpp:483` + `:487–:492`, measured 2026-09-03 over the 1,446 files that call a `runAot*` helper |
| `BEZEL_EXPECT_FALSE(<var>.ran)` sites, same scope | 205 | same |
| #3732's own headline figures | 1,008 + 84 = 1,092 | #3732, 2026-09-03 — **does not reproduce**; `aot_support.hpp:487–492` records that it counted all 3,015 `hasErrors()` in `tests/`, including the JIT/sema harnesses this path cannot reach |
| `hasErrors()` anywhere in `tests/**/*.cpp` today | **3,971** | grep, 2026-09-17 |
| `BEZEL_EXPECT_FALSE(<ident>.ran)` today | **333** | grep, 2026-09-17 |

**Finding from reading, which narrows this hole considerably.** Both shared helpers already exclude
timeout text: `failedAtCompileTimeWithRealLocation` rejects `"did not finish within"` and
`"did not terminate within"` explicitly (`tests/support/aot_result_asserts.hpp:130–137`), and
`failedAtCompileTimeNotRuntime` requires positive rejection text (`:113–:119`). Any row protected
by a positive substring check is likewise immune, because the four synthetic texts are fixed. So
**hole 4's vulnerable set is hole 3's set plus one delta**: rows whose only protection is a
**hand-rolled location check** — `.line > 0 && .column > 0` in the body, not via the shared helper.
Those are hole-3-protected (the location clause is protection (d) in
`check_ambiguous_ran_false.py:51–56`) but **not** timeout-proof, because a `TimedOut` diagnostic
appends whatever compiler stderr was captured before the kill
(`tests/support/aot_support.hpp:1001–1006`, the `"\n--- compiler stderr ---\n" + err` tail) and that
fragment can carry a real-looking location. This is exactly the hazard the shared helper's
belt-and-braces comment describes (`aot_result_asserts.hpp:130–137`).

Hand-rolled `.line > 0` checks today: **24** lines (`grep -rnE '\.line *> *0' tests --include=*.cpp`,
2026-09-17) — the delta is small, which is good news for the burn-down and bad news for anyone who
expected the source ratchet to be the main event here.

### What the ratchet asserts

**Source half — `scripts/check_timeout_satisfiable_negatives.py`.** Scans `tests/**/*.cpp`. One
banked count, `bare_location_rows`: a `(file, BEZEL_TEST, variable)` row that makes a rejection
claim, is **not** counted by `check_ambiguous_ran_false.py` (i.e. it is protected), and whose only
protection is a location check performed **in the body** rather than through
`failedAtCompileTimeWithRealLocation`. The row-identification machinery is the same; the script
should import it from `check_ambiguous_ran_false.py` rather than fork it, so the two predicates
cannot drift. Exempt: rows that also exclude at least one timeout literal (`"did not finish
within"`, `"did not terminate within"`) — those are hand-rolled but sound.

**Runtime half, and it is the real fix.** #3732 option 1: a third channel. At
`tests/support/aot_support.hpp:1001` and `:2495`, route `TimedOut` through a
`noteTimeoutFailure()` modelled line-for-line on `noteInfraFailure` (`:608–:628`):

1. mark the current test failed unconditionally, so **no assertion of any shape can read a timeout
   as green** — the #3007 argument at `:494–:503` applies verbatim;
2. record the reason at `Severity::Note`, never `Error`, so `hasErrors()` stays **false** and all
   272 + 205 sites become structurally immune in one edit rather than by a 477-row sweep;
3. tally into its own counter, printed unconditionally at exit as `TIMEOUT: N compile/run
   timeout(s)`, beside `INFRA: N failure(s)` (`:565–:578`) — **separate from the infra tally**, which
   is #3732's stated reason for a third channel rather than reusing infra: a real compiler hang is a
   finding, and folding it into the infra floor muddies exactly the signal that floor is for.

⚠ **Cost, stated rather than discovered later.** Under this change a timeout caused by *contention*
(CLAUDE.md: a cold `clang` link crossing 120s under 16 shards plus lane builds is "not exotic")
becomes a loud per-test failure instead of a silent green. That is the over-report class the gate
already has a protocol for — re-run the suspect alone with `BEZEL_TEST_FILTER=<name>` (CLAUDE.md,
`tests/README.md:137–141`), never a serial whole-suite pass. Trading a false green for a
disambiguable false red is the right trade, but it is a trade, and it is the kind of choice
CLAUDE.md rule 4 says must be recorded rather than made silently.

The `TIMEOUT:` line should **not** get a `run_parallel.sh` floor of its own: a timeout already fails
its test, so the run fails through the ordinary path. The tally is for triage.

### What the baseline file holds

`scripts/timeout_satisfiable_negatives.baseline`, a single total in the
`deprecated_jit_oracle.baseline` shape:

```
bare_location_rows = <N>   # seed: <= 24 (grep for `.line > 0`, 2026-09-17);
                           # to be re-measured at implementation time — the tree has moved
```

Exact, not a ceiling. One total rather than per-file rows because the expected size is one or two
dozen and there is no per-site triage to record — a hand-rolled location check either excludes the
timeout literals or it does not.

### What the self-test plants

In a temp tree: a row whose body checks `d.location.line > 0 && d.location.column > 0` and nothing
else (counts); the same row with an added `"did not finish within"` exclusion (does not count); a
row using `failedAtCompileTimeWithRealLocation(r)` (does not count); a bare `!ran` row (does **not**
count here — it belongs to hole 3, and the self-test must assert the two scripts partition rather
than overlap). Plus the five family assertions.

### How the gate phase invokes it

Phase name **`timeout-negatives phase`**, after the native-asan-leg phase. Log:
`$LOGDIR/${TAG}_timeoutneg.log`.

```
say "timeout-negatives phase: detector self-test + ratchet (#3732)"
  self-test  -> CYCLE FAILURE: check_timeout_satisfiable_negatives SELF-TEST failed —
                the detector is broken, so its verdict on the tree means nothing.
  --strict   -> CYCLE FAILURE: a new row pins a compile-time rejection with a
                hand-rolled location check that a compile TIMEOUT can satisfy — a
                negative test can go green because the harness gave up (#3732).
  green      -> say "timeout-negatives phase: GREEN (...)"
```

### Burn-down

1. **Land the channel first.** Once `TimedOut` reports at `Severity::Note` and fails its own test,
   every `hasErrors()` and `!ran` row is immune, including the ones nobody enumerated. The 477-site
   figure never needs to be swept.
2. **Then the delta**: each hand-rolled location row becomes
   `failedAtCompileTimeWithRealLocation(r)` (`tests/support/aot_result_asserts.hpp:138–154`), which
   is the same check plus the four exclusions. Bank with `--update`.
3. `tests/sema/catch_multitype_no_common_base_v182_test.cpp` pins the **absence** of a timeout by
   searching diagnostics for `"did not finish within"` (#3732 names it at its lines 360–366). It
   searches every diagnostic regardless of severity, so the demotion to `Severity::Note` does not
   break it — **verify this by reading before landing the channel**; it is the one row whose
   meaning the channel could silently change.

**Done criterion:** `bare_location_rows = 0` **and** the channel landed. Then **retire the script**
— unlike holes 1, 2 and 3, the shape it counts stops being dangerous once the harness cannot emit a
timeout as an `Error`, so a zero-guard would be guarding against nothing. What replaces it is the
channel's own regression test, in the shape of `tests/support/aot_infra_loud_3007_test.cpp`: a
forced timeout (`BEZEL_AOT_COMPILE_TIMEOUT_SEC=1`, the knob #3732's repro sketch names — confirm the
env var's spelling against `aot_support.hpp:190` before relying on it) must fail its test and must
leave `hasErrors()` false.

### Falsification

- If a row exists that *legitimately* asserts on a timeout as a positive outcome, demoting to
  `Severity::Note` breaks it. The v182 test above is the known case and is believed safe; a second
  one would mean the channel needs an opt-in escape.
- If contention timeouts turn out to be frequent enough that the gate reds out routinely, the
  trade above is wrong and the deadline (`aotCompileTimeoutSec`, `:190`) is the thing to fix, not
  the channel.

---

## 5. Hand-rolled `emitModule` / `emitObjectFile` runners bypass `forceSanitizeAddress()` (#3917)

### The hole

`bezel::test::forceSanitizeAddress()` (`tests/support/force_sanitize.hpp:29–38`) is the ONE
predicate `--full` flows through: `gate.sh` exports `BEZEL_AOT_FORCE_SANITIZE=address`
(`scripts/gate.sh:316`), `runAot` ORs it into the codegen options
(`tests/support/aot_support.hpp:2419`), and `driver_run_support.hpp` does the same for hand-rolled
`Driver::compileFile` runners. A test that calls `codegen::emitModule` / `codegen::emitObjectFile`
**directly** never consults it, so `--full` does not instrument that program's generated code. The
concrete instance is `tests/codegen/record_string_field_call_release_1747_test.cpp:89` (the
`emitObjectFile` call with no 4th argument; the parameter defaults to `false` at
`src/codegen/LlvmBackend.hpp:102`, per the #3917 comment of 2026-09-06). LSan still catches a
**leak** there, because leak detection needs only the interposed allocator; a use-after-free
**read** by generated code is invisible, because no shadow-memory check is emitted.

| Quantity | Value | Source |
|---|---|---|
| files calling `codegen::emitObjectFile` | ~80, corrected to 87 | #3917 (2026-09-06) and its comment (same day) |
| … today | **88** files, **99** call occurrences | grep, 2026-09-17 |
| … of those, files never consulting `forceSanitizeAddress` | **87** | `grep -rl 'codegen::emitObjectFile' tests --include=*.cpp \| xargs grep -L 'forceSanitizeAddress\|sanitizeAddress = true\|sanitizeAddress=true\|opts.sanitizeAddress'`, 2026-09-17 — the issue's exact recipe |
| the one file that does mention it | `tests/codegen/record_string_field_call_release_1747_test.cpp:169` | and only in a **comment** explaining that it does not call it — read 2026-09-17 |
| files among the 88 carrying a `no_uaf` / `_uaf_` / `double_free` / `no_crash` row name | **42** | grep, 2026-09-17 |
| rows whose name overpromises, hand-confirmed | 2, both in the 1747 file | #3917 comment, 2026-09-06 |

That last pair of rows is the point: **42 files carry a row name that promises more than this
oracle can deliver**, and two have been individually confirmed. The other 40 are a lead, not a
finding.

### What the ratchet asserts

**`scripts/check_uninstrumented_emit_runners.py`.** Scans `tests/**/*.cpp`. Two banked per-file
counts:

- `bypassing_calls` — a call to `codegen::emitObjectFile(` with fewer than four arguments (so the
  `sanitizeAddress` parameter takes its `false` default) **in a file that nowhere reads**
  `forceSanitizeAddress()` or sets `sanitizeAddress` true. Argument counting must be
  paren-balanced, not comma-split (the existing `_balanced_paren` at
  `check_ambiguous_ran_false.py:303` is the implementation to reuse).
- `overpromising_rows` — the sharper count, and the one that matters: a `BEZEL_TEST` **name**
  matching `no_uaf|_uaf_|double_free|use_after_free|no_crash|no_corruption` inside a file with a
  nonzero `bypassing_calls`. This is the name-vs-oracle mismatch the #3917 comment demands be
  audited ("either move those rows onto an oracle that really instruments generated code, or rename
  them").

Exempt, and why: a wrapper in `tests/support/` that itself consults `forceSanitizeAddress()` is the
fix, not a violation — exempt by path, exactly as `driver_run_support.hpp` is today. A pure-leak row
is **not** exempt from `bypassing_calls` (the capability gap is real: `--full` does not instrument
it) but is expected to carry no `overpromising_rows` weight, which is why the two counts are
separate. As `check_localdecl_only_walkers.py:67–75` puts it for its own family, `bypassing_calls`
is **an upper bound, not a defect count**; `overpromising_rows` is the defect count.

There is **no runtime half** available for this hole, and that is worth stating rather than leaving
implicit: a program that never consults the predicate cannot count itself as uninstrumented. The
source shape is the only observable. (A partial runtime check — "under `--full`, at least N
programs reported instrumentation" — would be satisfied by the rows that already work and would say
nothing about the 87 files. It is not proposed.)

### What the baseline file holds

`scripts/uninstrumented_emit_runners.baseline`, per-file rows in the localdecl format, two
sections:

```
# bypassing_calls: emitObjectFile( calls defaulting sanitizeAddress=false in a
# file that never reads forceSanitizeAddress()
2    tests/codegen/record_string_field_call_release_1747_test.cpp
...
[overpromising_rows]
2    tests/codegen/record_string_field_call_release_1747_test.cpp
...
```

Seeds: `bypassing_calls` over **87** files (99 call occurrences tree-wide is the ceiling), and
`overpromising_rows` seeded from the **42** candidate files, of which **2** rows are hand-confirmed
(#3917 comment, 2026-09-06). Both **to be re-measured at implementation time — the tree has moved**
(the issue said ~80 files, then 87; today's grep says 88). Exact, not a ceiling, per file, with
`scripts/uninstrumented_emit_runners.grandfather` and `// EMIT-ASAN-DEBT: #NNNN <reason>` for a
justified rise, plus `// EMIT-ASAN-ONLY: <reason>` for a row whose claim genuinely needs no
generated-code instrumentation (a pure-leak row, where LSan alone is sufficient — the exemption
#3917 itself grants in its "Broader scope" paragraph).

### What the self-test plants

In a temp tree: a file with `emitObjectFile(*m, obj, err)` and a row named `..._no_uaf_...` (counts
in both sections); the same file with `emitObjectFile(*m, obj, err, bezel::test::forceSanitizeAddress())`
(counts in neither); a file with the three-argument call and only a `..._leak_...` row (counts in
`bypassing_calls` only); a `tests/support/` wrapper that reads the predicate (exempt by path); an
`// EMIT-ASAN-ONLY:`-marked row (does not count). Plus the five family assertions.

### How the gate phase invokes it

Phase name **`uninstrumented-emit phase`**, last of the four new phases. Log:
`$LOGDIR/${TAG}_emitasan.log`.

```
say "uninstrumented-emit phase: detector self-test + ratchet (#3917)"
  self-test  -> CYCLE FAILURE: check_uninstrumented_emit_runners SELF-TEST failed —
                the detector is broken, so its verdict on the tree means nothing.
  --strict   -> CYCLE FAILURE: a test lowers a program with emitObjectFile() without
                threading forceSanitizeAddress(), so `--full` does not instrument that
                program's generated code — a "no UAF" row there can only observe a
                signal death (#3917).
  green      -> say "uninstrumented-emit phase: GREEN (...)"
```

### Burn-down

1. **The wrapper first**, in `tests/support/`, mirroring `driver_run_support.hpp`'s
   `compileFileForTest` pattern (#3917's own suggested shape): `emitObjectFileForTest(module, path,
   err, sanitizeAddress = false)` that ORs in `forceSanitizeAddress()`. A normal (unset) run stays
   byte-for-byte unchanged; `--full` instruments.
2. **Repoint the 87 files** in batches, banking each with `--update`.
3. **Triage the 42 overpromising candidates** one at a time. Per the #3917 comment, each ends in
   one of three places: moved onto `runAotChecked` (the AOT harness's sanitizer path, which works
   in lanes), repointed at the wrapper, or **renamed** so the name stops promising UAF-freedom the
   oracle cannot deliver.
4. Pure-leak rows that are genuinely fine get `// EMIT-ASAN-ONLY:` and leave the count.

**Done criterion:** `overpromising_rows = 0` and `bypassing_calls` reduced to its marker-only
residue. **Keep the script as a zero-guard** — the three-argument call is the natural way to write
the call and `src/codegen/LlvmBackend.hpp:102`'s default is `false`, so the gap regenerates every
time someone hand-rolls a runner. #3156 already inventoried the sibling shape
(`Driver::compileFile`) and closed; this one was invisible to it precisely because it bypasses
`Driver` entirely.

### Falsification

- If most of the 42 candidate rows turn out to make only leak claims despite their names, the
  `overpromising_rows` predicate is name-driven and over-broad, and the count should key on the
  row's assertions rather than its name. The counter-evidence is that #3917's two confirmed rows
  were found exactly this way.
- If `emitObjectFile`'s signature changes so `sanitizeAddress` is no longer positional-defaulted,
  the arity test silently stops matching. The self-test must therefore assert on a fixture that
  mirrors the **current** signature at `src/codegen/LlvmBackend.hpp:102`, and the phase's failure
  message should name that file so the next reader looks there first.

---

## 6. Ordering and dependencies

#4452 suggests: **#3733 and #3682 first** (the silent-skip pair, smallest, and they gate whether
anything else can be *verified* fixed), then **#3732**, then **#3892/#3895** together.

**Confirmed, with two corrections from reading.**

| Step | Work | Why here | Depends on |
|---|---|---|---|
| 1 | #3682 runtime refusal + the one-line #3676 fix (`:188`, `-1` → `-3`) | It is the smallest change in the programme and it is what makes *a lane's own ASan evidence mean anything*. Until it lands, every burn-down below is verified by lanes whose ASan leg may be vacuous. | root-causing `asanRuntimeObject()` emptiness; nothing else |
| 2 | #3733 runtime tally (counter → `test_main.cpp` → shard JSON → `run_parallel.sh` floor) | Same reason, one level up, and it is a mechanical port of #3626's wiring, all four sites read and cited in §0.2. | nothing |
| 3 | All five **source ratchets** wired into `gate_full_cycle.sh` | Ratchet-before-fix (#4452 step 1). Cheap, build-free, and they freeze five surfaces that are all currently growing (hole 1's grew ~495 call sites in 14 days). | steps 1–2 only in the sense that their baselines should be measured on a tree whose oracles are honest |
| 4 | #3732 **channel** (`TimedOut` → `Severity::Note` + test failure + separate tally) | One edit at two sites (`:1001`, `:2495`) retires 477 vulnerable assertions. Doing it before the #3892 sweep removes one of the four outcomes the sweep has to reason about. | the v182 absence-test must be re-read first (§4 burn-down item 3) |
| 5 | #3892/#3895 sweep, 30 rows, plus completion items C1–C5 | The rows are independent; C1 (the suppression marker) is required **before** the baseline can reach 0. | step 4 makes the remedy simpler, not possible — the sweep can proceed in parallel if needed |
| 6 | #3917 wrapper + 87-file repoint + 42-row name audit | Largest mechanical surface, smallest soundness delta per row, and its verification (does `--full` now instrument this?) needs step 1's honest ASan leg. | step 1 |

**Correction 1 — #3682 before #3733, not alongside.** #3733's runtime half is verified by watching
a tally; #3682's is verified by watching a *test fail*, which requires the very leg #3682 says is
vacuous in lanes. Doing #3682 first means every subsequent verification is done on an honest
oracle. #4452 lists them as a pair; reading says the pair has an internal order.

**Correction 2 — #3732's ratchet is small and its channel is large.** #4452 describes #3732 as "one
channel, two assertion families" and sizes it at 477 assertions. Reading
`tests/support/aot_result_asserts.hpp:113–154` shows the two shared helpers **already** exclude
timeout text, so the sweep-able residue is the ~24 hand-rolled location checks, not 477. The
channel is the whole job. That makes #3732 *cheaper* than its headline, which is an argument for
keeping it at step 4 rather than deferring it.

**Harness changes that gate a burn-down** (a row cannot be fixed before the change lands):

- hole 4's per-row replacement is available today (`failedAtCompileTimeWithRealLocation`), so the
  delta rows are **not** gated on the channel — but the 477 sites' immunity **is**.
- hole 5's per-row replacement is gated on the `emitObjectFileForTest` wrapper existing.
- hole 3's replacement is already shipped (`tests/support/aot_result_asserts.hpp`, 737 lines mentioning it
  today), so nothing gates it except C1 for the final zero.
- hole 1's and hole 2's guard deletions are gated on their runtime halves: deleting a guard before
  the harness refuses turns a silent skip into a confusing failure on a legitimate box.

---

## 7. What this specification deliberately does not cover

- **#3901 and #3607 — the doc/conformance corpus proves "compiles", not "behaves."**
  `tests/README.md:183–187` records the measurement at `907ce0f`: of 467 programs that ran under
  both compilers, **only 24 printed anything** and 5 wrote to stderr, so "467 executed, 0 stdout
  mismatches" overstates its evidence by ~19×. That is a real oracle hole of the same family. It is
  excluded here because the remedy is **not a ratchet**: nothing about it can be frozen by counting
  a source shape. Fixing it means *writing observable output into hundreds of corpus programs*,
  which is a content programme with its own review question (what should each program print?), not
  a mechanical burn-down. Ratcheting "programs with no observable" would freeze a number that only
  falls when someone writes new assertions — the ratchet would be a to-do list wearing a gate
  phase. It belongs to a separate proposal.
- **A general audit of the ~1,446 files that reach the AOT harness** for other ambiguous shapes.
  Five measured holes are the scope; a sixth found by inspection goes to a new issue, per the
  standing policy.
- **Any change to `docs/` normative documents.** Nothing in this programme needs one. The only
  spec-adjacent surfaces touched are the harness's own claims about §28.2 (checked disposal, spec
  v0.306, `tests/support/aot_support.hpp:2542–2546`) and §33.5 (entry-point resolution, spec
  v0.306, referenced at `:2613`), and neither changes.
- **The deadline value** `aotCompileTimeoutSec()` (120s, `tests/support/aot_support.hpp:190`).
  Tuning it is a contention question, not an oracle question.
- **Retiring the SKIP contract for boxes that genuinely cannot run the suite.** #3733 calls this a
  policy decision about which environments may run the suite. The programme reaches it as a
  *consequence* (hole 1's done criterion), not as a premise.

---

## 8. Open questions for the implementer

Each is something reading could not settle, with the line it was read at.

**Q1 — Is there any box that runs this suite without `clang`?** `tests/support/aot_support.hpp:2645`
probes it once per process, and #3733 argues the AOT harness cannot function without it at all
(`Driver::compileFile` shells out for every link). If that is right, the runtime tally is
permanently 0 and hole 1's burn-down is mechanical. If some CI or container runs a subset without
`clang`, making the SKIP fatal is a policy change with a real cost. **Cannot be settled by reading**
— it needs one run of the tally on every box that gates.

**Q2 — Why is `asanRuntimeObject()` empty in a lane worktree?** The rt-objs cache directory is keyed
on `std::hash<std::string>{}(srcDir.string())` (`tests/native_test_harness.hpp:196–203`), i.e. **per
source tree by construction**, and `runtimeSourceDir()` prefers the absolute `BEZEL_RUNTIME_SRC_DIR`
define from `tests/CMakeLists.txt` with a `__FILE__` fallback (`:98–:113`). A lane's `build/` is a
symlink to `/mnt/fast/lane-builds/<name>` (CLAUDE.md), so the define's value and the cache key both
depend on how that lane configured. Which of the three candidates #3682 lists is actually firing
**cannot be determined without building a lane worktree**, which this environment cannot do.

**Q3 — RESOLVED by a later read (coordinator, 2026-09-17).** The compile-timeout variable is
spelled `BEZEL_AOT_COMPILE_TIMEOUT_SEC`, read by `aotCompileTimeoutSec()` at
`tests/support/aot_support.hpp:190–192` (`std::getenv("BEZEL_AOT_COMPILE_TIMEOUT_SEC")`; the
comment at `:188–189` says it overrides and `0` disables). #3732's repro sketch has it right.

**Q4 — Does `tests/sema/catch_multitype_no_common_base_v182_test.cpp` still pin the absence of a
timeout the way #3732 describes?** The issue cites its lines 360–366 as of 2026-09-03. That file was
**not opened** for this document. It is the one row whose meaning #3732's channel could silently
change (a `Severity::Note` demotion), so read it before landing step 4.

**Q5 — How many of the 165 negative-return guards are actually bound to an ASan-leg call?** The
figure comes from a **line-based** grep (`if (x == -1) return;` filtered to lines naming
`asan`/`san`), which neither binds the variable to its call nor sees a guard whose variable is named
something else (`lsan`, `rc`, `sanRc` — two of the five sampled hits use `lsan`). The real predicate
needs the binding pass hole 3's script already has. Treat 165 as an upper bound on one spelling and
a lower bound on the class.

**Q6 — RESOLVED by a later read (coordinator, 2026-09-17).** `src/codegen/LlvmBackend.hpp:102–103`
declares `bool emitObjectFile(llvm::Module& module, const std::string& objectPath, std::string&
error, bool sanitizeAddress = false);` — `sanitizeAddress` is the 4th positional parameter with a
`false` default, exactly as #3917's comment says, so the arity discriminator in §5 stands.

**Q7 — Does the existing `check_ambiguous_ran_false.py` row-identification machinery import
cleanly?** Holes 1, 4 and 5 all want its `_balanced_body` (`:151`), `_balanced_paren` (`:303`),
`_strip_comments` (`:207`) and `_text_checking_helpers` (`:347`). Importing rather than forking is
clearly right — the two predicates must not drift — but the script is a `__main__`-style module with
a module-level `ROOT`/`BASELINE` (`:90–:91`) and no package structure. Whether that wants a small
shared `scripts/testscan/` module (the way `scripts/gate/` is already a package with its own
`scripts/gate/tests/`) is a structural call this document does not make.
