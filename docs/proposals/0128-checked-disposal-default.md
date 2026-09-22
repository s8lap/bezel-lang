# Proposal 0128 — Checked disposal mode is the default build; unchecked mode is the explicitly selected release relaxation

| | |
|---|---|
| **Status** | graduated-v0.307 — superseded — graduated v0.307 (owner-directed 2026-09-22; adversarial critique received and every finding resolved 2026-09-22 (`scratch/finalize-A/resolutions.md`); landed with the v0.307 changelog entry) |
| **Tracking issue** | [#4602](https://github.com/s8lap/bezel-lang/issues/4602) (spec-ahead-of-impl, filed at landing) |
| **Target module(s)** | **spec** — §28.2 (*Use after disposal; checked disposal mode*: the two mode bullets and the "release-build relaxation" sentence), §31.4 (the `DisposedAccessException` paragraph's "during development" clause); **rationale** — a new §5.30 decision record (which also states the default's memory-retention cost); **code-generation reference** §4.1 (one paragraph) and its pin; **compiler** (spec-ahead-of-impl, tracked): `src/main.cpp`'s default and a new `--unchecked-disposal` switch; **conformance harness**: `flags:` allowlist (already carries `--unchecked-disposal`); no stdlib contact (stdlib §6.2's guard text names both modes without a default); no guide contact (Guide 01 §5 and Guide 02 name checked mode and "a release build" without stating a default — unchanged because still true) |
| **Written against** | spec **v0.306** (`cafdfc5`) |
| **Origin** | The 2026-09-22 "path of least resistance" investigation (make the good memory-management spelling the shortest one; make the compiler say what it already knows). Every trust admission in §28 names checked mode's `DisposedAccessException` as "the runtime backstop every trust admission names" (K4.1) — and the default build omits it. |

*Reference convention: unprefixed §-references are language-spec references; K tags are §28.3's kernel ids. Quotations are drawn at v0.306.*

---

## 0. Thesis

§28.2 defines two conformance modes and says which one the language is *about*: "A program that would raise `DisposedAccessException` in checked mode has undefined behavior in unchecked mode: unchecked mode is the release-build relaxation of the checked semantics, not a different language." Checked mode is the semantics; unchecked mode is a relaxation of it. Yet the same subsection makes the relaxation the default ("**Unchecked (release) mode** — use of a disposed instance is undefined behavior. This is the default."), so a program compiled with no flags gets the relaxation, and the backstop the ownership kernel leans on at every recorded trust clause — K4.1's "in checked disposal mode any later use through any alias faults deterministically as `DisposedAccessException`" — is present only when a build asks for it. §31.4 says as much in its own voice: the exception "exists to convert a heisenbug into a deterministic fault during development, and a build is free to omit it".

This proposal flips the default and nothing else: **a build that selects neither mode is a checked build; unchecked mode is selected explicitly** (`--unchecked-disposal`), keeping the two-mode structure, the equivalence claim, K4.1's posture, and the cost statement ("unchecked builds pay no cost for the mode's existence") word for word. The compiler's `--checked-disposal` switch stays accepted as a no-op so scripts keep working.

## 1. The current state, precisely

§28.2, *Use after disposal; checked disposal mode* (v0.306):

> Use of an instance after its first `Dispose()` is a program error. How the error manifests depends on the *disposal-checking mode*, a per-build choice between two conformance modes:
>
> - **Unchecked (release) mode** — use of a disposed instance is undefined behavior. This is the default.
> - **Checked mode** — any instance member access on a logically-disposed instance — an instance method call (including interface and virtual dispatch), a property or indexer accessor invocation, or a field access through the reference — raises `DisposedAccessException` ([§31.4](…)). A further `Dispose()` call remains a no-op, not an error, in both modes.
>
> The two modes give identical observable behavior for every program that never uses a disposed instance. A program that would raise `DisposedAccessException` in checked mode has undefined behavior in unchecked mode: unchecked mode is the release-build relaxation of the checked semantics, not a different language.
>
> Checked mode is implementable with state the language already requires: idempotent disposal implies a per-instance disposed flag, and the split between logical disposal and physical reclamation ([§28.12](…)) gives the implementation latitude to defer reclaiming a disposed instance's memory in checked mode (quarantine, delayed free) so that the flag remains readable after disposal. The check itself is a flag test on member entry; implementations are expected to omit it entirely in unchecked builds, so unchecked builds pay no cost for the mode's existence.

§28.3, K4 clause 1: "**The backstop.** Disposal is idempotent and flag-setting; in checked disposal mode ([§28.2](…)) any later use through any alias faults deterministically as `DisposedAccessException`, and unchecked mode is the same language with the check elided. This is the runtime backstop every trust admission names."

§31.4: "`DisposedAccessException` is raised only by implementations operating in checked disposal mode ([§28.2](…)); in unchecked mode the same access is undefined behavior. Programs must not rely on it for control flow — it exists to convert a heisenbug into a deterministic fault during development, and a build is free to omit it."

§22.16 (the *independent* contracts mode): "**`ensures` and `invariant` run under checked-contracts mode** — a per-build choice between two conformance modes, independent of [§28.2](…)'s disposal-checking mode: in **unchecked (release) mode** — the default — unproven `ensures` and `invariant` clauses are not evaluated; … the deliberate contrast with §28.2, whose release relaxation is undefined behavior."

**Every other statement about the modes, audited** (`grep -n -i "unchecked\|checked disposal\|checked mode\|checked-disposal" docs/*.md`, 2026-09-22, excluding the changelog): spec §22.7 (the construction watermark: `NullReferenceException` "in checked disposal mode … and, unchecked, observes the watermark `null` with undefined behavior" — no default stated, **unchanged**); §28.2's compiler-inserted-free paragraph ("unchecked builds pay nothing, per this section's existing posture" — **unchanged**, the posture is the cost statement, not the default); §28.2's scoped-callback backstop (**unchanged**); §28.3 K4.1 and K5 (ii) (**unchanged** — neither names a default); §28.4 (reclamation "deferred under checked disposal mode's quarantine latitude" — **unchanged**); §28.11, §28.12, §28.14, §29.11, §30.x, §32.8 (each names checked mode as the backstop and unchecked as undefined; none states a default — **unchanged**); stdlib §6.2 (`Locked<T>`'s guard: "checked disposal mode detects … unchecked it is undefined behavior" — **unchanged**); rationale §5.1, §5.28, §12.8 area, §16.4 (each "checked-mode deterministic fault, unchecked UB" — **unchanged**); Guide 01 §5/§10 ("a `DisposedAccessException` in checked disposal mode and undefined behavior in a release build" — **unchanged**: a release build is now the build that *selected* unchecked mode, and the sentence stays true); Guide 02 ("a deterministic fault in checked mode" — **unchanged**); Guide 05 §6 (the trap comments "checked mode: `DisposedAccessException`" — **unchanged**); the code-generation reference §4.2/§4.5 ("faults in checked mode" — **unchanged**, with one paragraph *added* at §4.1 so a model writing Bezel knows which build it is writing against). The two statements that assert or imply the default are the two edited: §28.2's bullet and §31.4's "during development". §22.16's "the default" names its *own* mode's default (contracts stay unchecked by default) and its "deliberate contrast with §28.2, whose release relaxation is undefined behavior" stays true — **unchanged**; the reason the two independent modes default differently is rationale (§5.30), not normative text (critique N-3).

**What the compiler does today** (§5 item 3b check). `src/main.cpp:130-135`: `--checked-disposal` sets `codegenOpts.checkedDisposal = true`; the usage text (`:41-44`) describes it as "enable checked disposal mode (§28.2)". `src/codegen/LlvmBackend.hpp:31-37`: `bool checkedDisposal = false;` with the comment "Default false = unchecked (release) mode: zero extra cost." No `--unchecked-disposal` switch exists (`grep -rn "unchecked-disposal" src/` → 0 before this train; the corpus `flags:` allowlist in `tests/conformance/rules/corpus.py` already names it). The test harness: `tests/support/aot_support.hpp:3246-3250`, `runAotCheckedDisposal`, sets the one bit; `runAot` runs the library default (unchecked); `runAotChecked` (`:3534`) runs the library default under ASan **with LeakSanitizer on** (`detect_leaks=1`, `:3457`); the forced-ASan sweep (`--full`) runs every `runAot` program with `detect_leaks=0` (`:3117-3127`). 22 test files use `runAotCheckedDisposal`/`checkedDisposal = true` (`grep -rl … tests/` → 22, of which 4 are harness/support files). Two corpus cases carry `flags: --checked-disposal` (`s28_10_0012`, `s31_4_0006`).

## 2. The rule (exact normative wording)

§28.2's two bullets become (order swapped so the default is stated first, exactly as §22.16 states its own):

> - **Checked mode** — any instance member access on a logically-disposed instance — an instance method call (including interface and virtual dispatch), a property or indexer accessor invocation, or a field access through the reference — raises `DisposedAccessException` ([§31.4](#sec-standard-exception-types)). This is the default: a build that selects neither mode is a checked build.
> - **Unchecked (release) mode** — use of a disposed instance is undefined behavior; a build selects it explicitly. A further `Dispose()` call remains a no-op, not an error, in both modes.

§28.2's relaxation sentence becomes:

> A program that would raise `DisposedAccessException` in checked mode has undefined behavior in unchecked mode: unchecked mode is the release-build relaxation of the default checked semantics — selected per build, never implied — not a different language.

§31.4's clause becomes: *"it exists to convert a heisenbug into a deterministic fault, and a release build that selects unchecked mode omits it."*

Everything else in §28.2 — the equivalence sentence, the implementability paragraph, the cost statement, the compiler-inserted free, the warning paragraph, the scoped-callback backstop — is unchanged. K4.1's posture is unchanged. The exact `old → new` pairs are in `scratch/train-A/edits.md` (group B).

### 2.1 The compiler: CLI default flips; `--unchecked-disposal` added; `--checked-disposal` kept as a no-op

- `bezelc` with neither switch builds in checked mode.
- `--unchecked-disposal` (name fixed) selects the release relaxation.
- `--checked-disposal` is accepted and does nothing (it names the default). Kept so scripts and the two flagged corpus cases keep working; its usage line says it is the default.
- Both switches given: the last one wins? **No** — reject with a usage error naming both; a build's mode should never depend on argument order. (Stated as a choice the spec does not make; §9 Q1.)

### 2.2 The Driver library API default: **unchanged in v1** (`CodegenOptions.checkedDisposal = false`), and why

The question is whether `codegen::CodegenOptions::checkedDisposal`'s default should flip with the CLI. Recommendation: **the CLI flips; the library struct keeps `false`, with its comment rewritten to say that the conformance default lives at the compiler's user-facing entry and that an embedder chooses explicitly.** Three grounds, the third decisive:

1. **What the spec's "default" is.** §28.2 says "a per-build choice"; the build is what `bezelc` produces. A library embedder that constructs `CodegenOptions` is not "a build that selects neither mode" — it is a caller filling in a struct — and the struct's zero-value is an API fact, not a conformance statement. The conformance runner (`tests/conformance/rules/runner.py`) drives `build/bezelc`, so the corpus sees the spec's default.
2. **The plain gtest oracle keeps its meaning.** `runAot` runs the library default. If that flipped, every one of the suite's AOT tests would run checked, and the plain oracle would silently change from "the release build" to "the checked build". That is *not* desirable for the oracle, because checked mode's quarantine **suppresses the heap free** (`LlvmBackend.hpp:35-36`; `LlvmBackend_impl.hpp:10440-10443`): a block that is never freed is a block AddressSanitizer can never see a use-after-free on. Checked mode catches member-entry accesses through the flag test; it does not catch a raw load through a dangling reference the codegen itself emits — which is exactly the defect class the `--full` ASan oracle exists to catch (#2633's whole argument: the two oracles catch different things). Running the default build checked would weaken the ASan oracle for the codegen's own bugs while claiming to strengthen it.
3. **`runAotChecked` runs LeakSanitizer.** The curated ASan call sites run with `detect_leaks=1` (`aot_support.hpp:3457`, `:3534`) and are "leak-clean by construction". Under the quarantine, every disposed instance is a never-freed block: with the library default flipped, every `runAotChecked` test would report leaks. Flipping the library default therefore requires either pinning `checkedDisposal = false` inside `runAotChecked`/`runAotCheckedImpl` (so the plain and the ASan oracles diverge on a bit the tests cannot see), or an LSan-visible quarantine list in the runtime — a runtime change this proposal does not want to carry.

So: the ASan `--full` oracle is unaffected (it forces `sanitizeAddress`, an orthogonal bit, and runs `detect_leaks=0`); the plain oracle keeps meaning "the release relaxation, with real frees"; `runAotCheckedDisposal` remains the checked-mode oracle. Harness follow-on, not blocking: a suite-wide `BEZEL_AOT_FORCE_CHECKED_DISPOSAL` override on the model of `forceCheckedContracts` (`tests/support/force_sanitize.hpp:46-54`) would let a gate run the whole suite checked as a *third* sweep; it should run with `detect_leaks=0` for the reason in item 3.

The honest cost of this split: the library and the CLI disagree on their zero-value, and a future embedder could be surprised. The comment on the struct is the mitigation; a rename (`uncheckedDisposal`, default `false`, meaning checked) would fix it structurally and is recorded as a follow-on for the compiler lane (§9 Q2), not a spec matter.

## 3. False-positive / rejected-shape analysis — what must NOT change verdict

The change is a runtime-mode default; no compile-time verdict moves. What must not change:

| Shape | Why unchanged |
|---|---|
| every program that never uses a disposed instance | §28.2: "The two modes give identical observable behavior for every program that never uses a disposed instance" — the default flip is unobservable to it |
| a program with `flags: --checked-disposal` (corpus `s28_10_0012`, `s31_4_0006`) | the switch names the default; the cases keep their verdicts |
| the §28.2 use-after-dispose *warning* | a compile-time rule, mode-independent — it fires in both builds and its text already names both outcomes |
| the K5 (ii) freeze criterion | reads "the disposed flag it guarantees is an admissible backstop" — the flag is set in both modes; only the *check* is elided in unchecked builds |
| §22.7's construction watermark | names checked mode explicitly; its default follows §28.2's |
| §22.16's checked-contracts mode | independent per-build choice; its default stays **unchecked** — the edited contrast clause says why |
| the compiler-inserted free's cost statement | "unchecked builds pay nothing" — still true of the build that selects unchecked |
| stdlib §6.2's lock-guard fault | "checked disposal mode detects … unchecked it is undefined behavior" — no default stated |

**Performance posture, stated — including the retention cost (critique F-7).** The default build now pays the flag test on member entry and the quarantine. §28.2 already calls the check "a flag test on member entry" and the quarantine "latitude"; the release relaxation is one switch away and is the documented way to shed both. No new cost is introduced; a cost the language already defined moves from opt-in to opt-out. What that cost *is* must be said plainly: checked mode's fault is deterministic only while the disposed flag stays readable through every alias, and without a liveness oracle that is the process's life — so a checked build **retains per-disposed-instance state until exit**. The reference compiler retains the *whole* disposed block: `src/codegen/LlvmBackend.hpp:31-37` ("suppresses the heap-free in emitDisposeAndReclaim (quarantine …)"), `src/runtime/bezel_runtime.h:121-124` ("does NOT free the header block"), `src/runtime/bezel_runtime_refcounting.cpp:396` ("Quarantine: do NOT free(c). The cell becomes a deliberate leak"); no reaping path exists. A service built with no switch therefore grows for as long as it disposes. Two things follow. (1) A *windowed* quarantine — reclaim a disposed block after N later disposals — is **declined**: reusing the block turns K4.1's "faults deterministically" into "faults within a window", which is the unchecked relaxation made probabilistic and contradicts §28.2's "raises"; §28.2's "latitude to defer" permits deferral, not reclaiming a flag an alias can still reach. (2) The retention can be reduced without weakening the fault — release the payload at disposal and keep the flag's footprint, never handing a disposed instance's address to a live one — and the tracking issue (lane report (d) issue 2) makes that reduction a **precondition of `bezelc` exercising the new default**, so the whole-block leak never becomes what a user gets without asking. Rationale §5.30 and the changelog's Migration note both disclose the cost; `--unchecked-disposal` remains the release form for a program that cannot carry any retention.

## 4. Alternatives considered and declined

1. **Status quo** (unchecked default). Declined: the language's soundness story is written against the checked semantics — every K4.3 recorded-trust clause, §28.14's "checked mode is defense-in-depth", §29.11's "the checked-disposal backstop remains the guard everywhere the warning is silent", §15.4's and §28.11's "backstopped by checked disposal mode" — and a default build that omits the backstop makes each of those sentences describe a build the user did not get. §31.4's own "during development" concedes the mode is the one a developer wants; a default should be what a developer wants.
2. **Flip the Driver library default with the CLI.** Declined for v1 on §2.2's grounds (the ASan oracle's real frees; `runAotChecked`'s LeakSanitizer). Recorded as a compiler-lane follow-on with the rename that makes the zero-value honest.
3. **Make checked mode the only mode** (delete the relaxation). Declined: §28.2's cost statement is load-bearing for a language whose §1 promise is "costs that follow from what the source says", and the quarantine's memory latitude is not free; the relaxation stays, explicit.
4. **Rename the switches** (`--release`, `--debug`). Declined: the name `--unchecked-disposal` is fixed by the train's brief, and it names what it does — the mode, not a build profile; the `--debug` flag already exists for `Debug.*` elision (stdlib §8) and means something else.
5. **Warn at compile time when the unchecked switch is given.** Declined: a deliberate switch is not a hazard; the §5.28 doctrine lints statically-certain hazards in *programs*, not build configurations.

`DECLINED.md` index check (2026-09-22): no row concerns the disposal-mode default. The rows that name checked mode (*Compile-time borrow-liveness checking*; *Keying the use-after-dominating-`Dispose()` diagnostic on the use form*) are untouched — both rely on the checked backstop existing, which this proposal makes the default rather than the exception.

## 5. Kernel and body-locality classification

- **Kernel.** No derivation and no axiom change: K4.1 states the backstop's *existence* in checked mode and its elision in unchecked mode; which build is the default is a conformance-mode statement in §28.2, outside the kernel's clauses. The kernel text is not edited.
- **Body-locality.** Not applicable — no compile-time rule is added or altered.

## 6. Migration and compatibility (changelog house categories)

- **Newly defined:** the default disposal-checking mode is checked; unchecked mode is selected explicitly per build. A program that uses a disposed instance and was compiled without a switch now faults deterministically with `DisposedAccessException` where it previously had undefined behavior — the one observable change, and it is the change from "anything" to "a defined fault".
- **Newly rejected / newly warned:** nothing.
- **Doc-only:** §31.4's clause; rationale §5.30; the code-generation reference's §4.1 paragraph and pin. (§22.16 is unchanged — critique N-3.)
- **Known cost, disclosed:** a checked build retains per-disposed-instance state for the process's life (§3); in the reference compiler today that is the whole disposed block. The tracking issue makes bounding the retention to the flag's footprint a precondition of the `bezelc` default flip; a windowed quarantine that reuses blocks is declined (§3).
- **Spec-ahead-of-impl:** `src/main.cpp` (the default and the new switch) — tracking issue in the lane report, with the diff. The corpus's `flags:` allowlist already admits `--unchecked-disposal`.

## 7. Implementation sketch

- `src/main.cpp:130-135` — `codegenOpts.checkedDisposal = true;` becomes the initial value before argument parsing; the `--checked-disposal` branch keeps the bit `true` (no-op); a new `--unchecked-disposal` branch sets it `false`; both given → usage error. Usage text (`:27`, `:41-44`) lists both switches and marks checked as the default.
- `src/codegen/LlvmBackend.hpp:31-37` — the struct default stays `false`; the comment is rewritten (the conformance default is the CLI's; an embedder chooses).
- `tests/support/aot_support.hpp` — no change required; `runAotCheckedDisposal`'s comment ("This is exactly `bezelc --checked-disposal`") stays true; optionally the `BEZEL_AOT_FORCE_CHECKED_DISPOSAL` sweep switch (§2.2).
- **Precondition of the CLI flip (F-7):** checked mode's per-instance retention is bounded to the flag's footprint — the payload is released at disposal and a disposed instance's address is never reused for a live one — so that the fault stays deterministic and the default build does not leak whole objects. Runtime/codegen work (`emitDisposeAndReclaim`, `bz_array_mark_disposed`, `bz_dict_mark_disposed`, the closure cell path), with an RSS probe (10⁷ create-and-dispose of a destructor-bearing class under the default) as its test. Size: **M**, and it gates the `main.cpp` change, not the spec text.
- Tests: a driver test that `bezelc` with no switch raises `DisposedAccessException` on a disposed member access (the corpus case `s28_2_0011` is the same program); `--unchecked-disposal` accepted; both switches rejected; `--checked-disposal` accepted as a no-op. Size: **S**.

## 8. Corpus accounting

| Case | Expectation | What it pins |
|---|---|---|
| `s28_2_0011` | `run`, `stdout: faulted`, **no `flags:`** | the default build is checked: a read through a borrow-pin after the source's explicit `Dispose()` raises `DisposedAccessException`, caught and reported — the shape §28.10 and the code-generation reference §4.2 already describe ("a later read through `y` faults in checked mode"), here with no switch |
| `s28_2_0012` | `accept`, `flags: --unchecked-disposal` | the same program compiles under the explicitly selected relaxation; its fault is undefined and therefore not observable, so the case asserts acceptance only |
| `s28_10_0012`, `s31_4_0006` (existing, `flags: --checked-disposal`) | unchanged | the switch is a no-op naming the default; both keep working |

`s28_2_0011` is the case that fails today (the default build prints `live`), which is the spec-ahead-of-impl delta the tracking issue names.

## 9. Open questions

- **Q1 — both switches given:** reject with a usage error (recommended) or last-wins?
- **Q2 — the library zero-value:** keep `checkedDisposal = false` with a rewritten comment (recommended for v1), or rename the field so that the zero-value means the default (`uncheckedDisposal = false`) in the same compiler change? The rename is the honest end state; it touches every construction site of `CodegenOptions` in `tests/` and the cache-key serialization (`aot_support.hpp:2057`, `:2178` — the `cd=` bit), so it is a compiler-lane decision.
- **Q3 — a suite-wide checked sweep:** worth adding as a third gate oracle (`BEZEL_AOT_FORCE_CHECKED_DISPOSAL`, `detect_leaks=0`)? It would exercise the default build across the whole suite, which no oracle does today.
- **Q4 — §22.16's contrast clause:** resolved — the addition is withdrawn (edit B4 retired; critique N-3); the reasoning lives in rationale §5.30 only.
- **Q5 — the retention bound (F-7):** the precondition in §7 asks for a flag-footprint stub with no address reuse. Whether the reference allocator can shrink a block in place, or the header must be split from the payload, is the compiler lane's design question; the spec constraint is only that the fault stays deterministic.
