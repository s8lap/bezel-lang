# A scripting tier for Bezel without a garbage collector, and a direct-emit backend (2026-09-29)

*Research note. Non-normative. Written in a remote session with no LLVM and no
build tree: every number below is either counted from the tree with the recipe
shown, quoted from a document or an in-tree comment (cited by file and line at
`main` `8bfdd88`), or marked as an estimate from general knowledge. Nothing
was built or run. The memory-model half builds on proposals 0127–0132 and the
v0.307 changelog entry; an adversarial critique pass over the first draft
found six blocking defects, all corrected here (the record is in the landing
PR).*

## Summary

The question: can a simplified Bezel become a scripting language — run from
source, no garbage collector — and can its compiler drop LLVM for a direct code
emitter? Six findings, each of which the tree supports rather than merely
permits, and one of which is a warning rather than an encouragement:

1. **The ownership *rules* are checked in the front end; the disposal
   *placement* is re-derived in the backend.** The pipeline is lex → parse →
   type-check → ownership-check → codegen (`src/driver/Driver.hpp:3-6`), but
   the ownership pass is a verifier that annotates the AST with a dozen
   flags (`src/ast/Node.hpp:137-192`: `autoDropAtScopeEnd`,
   `usingVarSourceIsBorrow`, and their siblings), and the §28 discharge
   machinery — scope-end disposal, drop-on-overwrite, unwind disposal, the
   owned-field cascade — "stays in codegen and is NOT moved" out of it
   (`src/common/OwnershipFacts.hpp:23-26`). `src/codegen/` contains no
   reference to the ownership pass at all (recipe in §4). So a second backend
   does **not** inherit "disposed exactly once" for free: it must
   re-implement what `LlvmBackend_Disposal.cpp` and `LlvmBackend_Assign.cpp`
   do today (23,589 lines between them), consuming the same AST flags and the
   shared expression-shape core. The right move is to hoist that placement
   into a backend-independent pass *before* writing any second backend — §6
   makes it the enabling stage, and it is worth doing even if no scripting
   tier follows, because the tree already records the drift class that two
   copies of one decision produce (`OwnershipFacts.hpp:5-11`, #1388/#1415).

2. **The spec already draws the inner boundary.** Appendix D defines a
   **Core** conformance level that excludes async, domains, channels, events,
   serialization synthesis, FFI, buffers/spans, contract *proving*, LINQ,
   `Shared<T>`/`Weak<T>` and move-only value types, and states that a Core
   implementation "satisfies the domain guarantees by construction" (D.4).
   A scripting tier is Core minus a short list (§2), plus one syntax addition
   (top-level statements), plus three v0.307 tools that make stored borrows
   cheap to get right. Those three tools are **spec-ahead-of-implementation**
   today (#4601, #4604, #4605) — the tier depends on compiler work already
   tracked, not on new design.

3. **The backend is where the weight is, and LLVM's reach is wider than the
   backend.** `src/codegen/` is 184,321 lines — 49% of the 374,472 under
   `src/` — against 94k for sema, 42k for ownership, 8k for the parser and
   0.9k for the lexer (§4). LLVM is also used at the driver's codegen seam
   (`Driver.cpp:34-35`, `:611`), and by `src/module/` for the `.bezel.meta`
   artifact format (`ModuleMetadata.cpp:27-28` uses LLVM's bitstream
   reader/writer; `ModuleLoader.cpp:25-28` uses `llvm/Object`). A
   scripting runner that emits no artifact can avoid the module code path,
   but "zero LLVM" for the whole toolchain is a larger claim than "a
   non-LLVM backend", and this note makes only the second.

4. **Bezel's codegen conventions are the ones a naive emitter wants.**
   Exceptions do not use landing pads or a personality routine: a throw
   stores the exception in a thread-local slot `bz_exc`
   (`bezel_runtime_concurrency.cpp:2722`) and returns, and codegen emits the
   pending-check branch after each call — the CG20 `unwindBB`/`propagateBB`
   convention (`LlvmBackend_impl.hpp:2679-2697`); the runtime header's own
   description of a throw helper is "stores … in the … exception slot
   (bz_exc) and returns (codegen emits the unwind branch after the call)"
   (`bezel_runtime.h:2100-2101`). Virtual dispatch reads a codegen-emitted
   vtable at slot 0 of the object (`LlvmBackend_Layout.cpp:2050`); interface
   dispatch is a runtime itable lookup (`bezel_runtime_itable.cpp`). Strings,
   closures and boxes are reference-counted through runtime calls
   (`bezel_runtime_refcounting.cpp:1`). Generics are monomorphized —
   "this implementation monomorphizes; proposal 0103 records the choice"
   (`LlvmBackend_impl.hpp:2820`). None of that needs an optimizer, SSA, or
   unwinding tables. The one hard lowering is the iterator/async state
   machine (§4 recipe); async is Extended and out, and synchronous iterator
   methods are the one Core feature this note recommends deferring from the
   tier's first version.

5. **The runtime is reusable as-is.** It is 23,589 lines of C++ behind a C ABI
   (471 lines of `bezel_runtime.h` declare a `bz_*` function; recipe in
   §4). The files a scripting tier needs — strings, refcounting,
   collections, file I/O, encoding, time, decimal/guid, itable, and
   shared/weak (since §2 keeps `Shared<T>`) — total 9,558 lines; the files it
   drops — concurrency, sync, events, serialization — total 8,940 and map onto
   Appendix D's Extended features. Whether the first set links without the
   second is probe 5 in §7.

6. **The existing suite is a differential oracle for a second backend.**
   There are 24,972 `BEZEL_TEST(` sites under `tests/` and 2,184 rule-corpus
   cases at v0.307. A second backend selected by a driver option can be run
   against both, with the LLVM backend's outputs as expected values and the
   corpus's document-stated verdicts catching the cases where both backends
   are wrong together. This is the strongest argument for doing the work
   inside this repository rather than as a fresh project: a fresh project has
   no oracle.

The recommendation (§6) is a staged path: hoist disposal placement out of the
backend first, gate the subset, then add a **C-emitting backend** consumed two
ways — in-process by a small C compiler for run-from-source, and by
`clang -O2` for release builds — with a direct machine-code emitter as a later
specialization only if the C step proves to be the bottleneck. The reasons,
and the honest costs of each option, are in §5.

---

## 1. What "scripting language" has to mean here

Three properties, in the order a user notices them:

- **Run from source in one command, fast.** `bzs script.bzl args` should
  compile and start in well under a second for a thousand-line script,
  cold. The only per-program figure the tree records is 0.12–0.26 s per
  test program (`docs/research/2026-08-10-dual-oracle-gate-investigation.md`,
  quoted in the 2026-08-19 audit's anchor table), which includes the LLVM
  pass pipeline and an external `clang` link with the system linker; the
  audit's F15 estimates that a faster linker alone would save ~100–300 ms
  per cold link. The link step is the first thing a scripting path removes.
- **Small programs need no ceremony.** Bezel already delivers this for the
  common case: a class with no destructor, no owned non-trivial field and no
  owning inline array is trivial-dispose and is freed silently at its owner's
  release point (spec §28.9). `var sb := new StringBuilder();` needs nothing.
  What stays announced is disposal *with an effect* — a file closing — and
  rationale §5.28 item 5 records why: the ceremony is not load-bearing for
  correctness, it is the announcement of an observable effect, and a class
  whose disposal has none sheds it.
- **Deterministic memory without a collector.** Kept unchanged: K1–K9 of
  §28.3, checked disposal mode (the spec's default since v0.307; the
  compiler's default is still unchecked until #4602 lands —
  `LlvmBackend.hpp:37`), and the runtime-managed tier (string, function
  values, boxes) whose reclamation the spec leaves to the implementation and
  the runtime implements with reference counts. A script's object graph is a
  tree of single owners with borrows, exactly as a service's is; the
  difference is only which features are reachable.

What a scripting language does **not** have to mean: dynamic typing. The
ownership classification of every expression is computed from its static type
(§28.3's producers table); a dynamically typed variant would have nothing to
classify. The tier is statically typed with inferred locals (`var`, spec
§21.1) and annotated boundaries (rationale §5.5), which Bezel already has.

## 2. The subset: Core minus a short list, plus one addition

Appendix D's Core is the starting point. The tier removes from Core:

| Removed from Core | Why | What a script uses instead |
|---|---|---|
| User-declared destructors (§28.4) | Every user class becomes trivial-dispose, so silence is honest by §28.9's own criterion. Resources stay stdlib classes with destructors, bound by `using var`. | `using var f := new FileStream(path, FileMode.Open, FileAccess.Read);`; a user class holding a resource marks the field `owned`, becomes non-trivial by structure, and draws the must-use warning — the derived-ceremony rule unchanged |
| Iterator methods (`yield`, §29.3–§29.8) | The only Core lowering that needs a state machine (finding 4). `for` over collections, ranges and strings stays. | Collections, `Drain()`, ranges; a later version adds `yield` once the emitter is stable |
| Extension blocks (§26), operator overloading (§25), user-declared generics (§14) | Each is a sema surface a script rarely needs and an emitter must specialise for. Stdlib generic *instantiations* (`Array<int>`, `Dictionary<string, T>`) stay — §5.4 on monomorphization. | Plain methods; the stdlib collections |
| Contract clauses (§22.16) and `pure` (§22.15) | Runtime-checked contracts are Core and proving is Extended; a script needs neither, and dropping both removes a checker and a lowering. | — |
| Multi-module programs (§2.8 artifacts) | A script is one compilation unit plus the stdlib prelude. | `import` of stdlib modules only, in version 1 |

Everything else in Core stays: nullability, records, enums, choices with `case`,
classes, single inheritance and interfaces, properties and indexers, lambdas,
exceptions, ranges, string interpolation, and the whole of §28 minus the
carve-outs above. From the Extended list the tier *adds back* exactly one
feature: **Shared ownership** (`Shared<T>`/`Weak<T>`, §28.13), because it is
the recorded escape hatch for the residue single ownership cannot express
(two long-lived owners; observer graphs), and rationale §5.21 declined every
implicit alternative. With `OwningSlotMap<T>` (stdlib §4.21) for graphs with
deletion and `scoped` on primary-constructor parameters (§22.10) for the
checked back-reference, the three tools proposals 0127, 0130 and 0131 named
are all present in the tier's *specification*; their implementation is
#4601, #4604 and #4605.

**The one addition: top-level statements.** §33 requires a `Main` method on a
class (the compiler resolves it by scanning; there is no `--main-class`
option today — `src/sema/EntryPoint.cpp:110`). A script wants
`Console.WriteLine("hi");` at file scope. The desugaring is mechanical and
needs no new semantics: a file whose top level contains statements is wrapped
in an implicit module and an implicit class whose `Main(args: Array<string>)`
body is the statement list, in order, with top-level declarations hoisted
beside it. Ownership rules apply to that body as to any method body — a
top-level `using var` is disposed at end of script — which is the behaviour a
user expects. This is a design change under spec-change-process §2 and would
be its own proposal; nothing else in this note changes the language.

**Feature gating is Appendix D's rejection discipline, not a fork.** D.1
requires an implementation that omits a feature to *reject* its use "with a
diagnostic naming the construct and the feature". A `--tier script` driver
option that rejects the constructs in the table above, by name, keeps one
grammar, one sema, one ownership checker and one spec. The tier is a
conformance claim of the "Core minus named features" shape, the mirror of the
"Core plus named features" claim D.1 already allows; stating that mirror is
the second thing a proposal would have to say.

## 3. What survives of the memory model, precisely

Every rule of §28 that is Core survives unchanged; this section lists what the
tier's removals *simplify*, so the reader can see that nothing load-bearing is
lost.

- **Teardown order becomes unobservable.** With no user destructors, the
  cascade over `owned` fields runs no user code, so sibling borrows under one
  owner cannot observe a half-torn-down parent. The rules stay (the stdlib's
  destructor-bearing classes still exist), but the class of bugs they guard
  against cannot arise in user code.
- **The residual-trust admissions stay, and stay backstopped.** A parameter-
  sourced borrow stored in a plain field is still §28.3's trust clause with
  checked mode as its backstop. For scripts checked mode is simply always
  on: the retention cost recorded in rationale §5.30 (the reference
  compiler keeps every disposed block) is bounded by a script's lifetime,
  and `--unchecked-disposal` (#4602) exists for the program that outgrows the
  tier. The trust-admission report (`--report-trust-admissions`, #4605) is
  the tool that lists what a script is trusting.
- **Domains vanish.** D.4 already states it: no `async`, one continuation,
  one domain, and the domain clause of K4.3 — the one admission with no
  runtime backstop at all — cannot arise.
- **The K8 boundary and the store lint stay.** `groups.Add("odds", [1, 3, 5])`
  still dies at statement end and still warns (§15.4). Scripts hit this
  shape often; the warning's remedy (bind the value first, or use the owning
  family) is one line.

What the tier gives up is exactly what a trivial-dispose-only Bezel gives up:
graphs with deletion go through handles, shared caches through `Shared<T>`, and
long-lived collections of borrows do not exist. Those are the model's limits,
not the tier's.

## 4. Sizing the compiler: what is reused and what is replaced

Line counts, measured 2026-09-29 at `main` (`8bfdd88`):

```
for d in src/*/; do printf "%-18s %7s\n" "$d" \
  "$(cat $(find $d -name '*.cpp' -o -name '*.hpp' -o -name '*.h') | wc -l)"; done
```

| Component | Lines | Scripting tier | Notes |
|---|---:|---|---|
| `src/lexer/` | 928 | reuse | — |
| `src/parser/` | 7,929 | reuse | plus the top-level-statement wrapper |
| `src/sema/` | 94,027 | reuse, gated | the Extended and removed features reject by name (D.1) |
| `src/ownership/` | 41,583 | reuse unchanged | the checker; see finding 1 for what it does and does not decide |
| `src/flow/`, `src/ast/`, `src/common/`, `src/support/` | 9,853 | reuse | `OwnershipFacts` is the shared expression-shape core both consumers query |
| `src/module/` | 13,000 | mostly bypassed | the prelude is 23 source lines injected by the driver (`Driver.cpp:62-101`), not loaded through this code; artifacts use LLVM bitstream |
| `src/driver/` | 1,247 | extend | there is no backend seam today: the driver calls `codegen::emitModule` and receives an `llvm::Module` (`Driver.cpp:611`, `LlvmBackend.hpp:102`) |
| `src/codegen/` (LLVM) | 184,321 | **replaced for the tier** | kept for release builds and as the differential oracle |
| `src/runtime/` | 23,589 | reuse 9,558 / drop 8,940 | finding 5; the C ABI is the contract |

Two observations follow. First, the reused front end is roughly 165k lines
that already exist and are tested; the tier writes none of it. Second, the
replaced backend is the single largest component, and — after finding 1 — a
material part of it is *not* replaceable by a printer: the disposal placement
(`LlvmBackend_Disposal.cpp`, 9,022 lines; the drop-on-overwrite and field
paths in `LlvmBackend_Assign.cpp`, 14,567) encodes §28's discharge rules a
second time. Hoisting that into a backend-independent "release plan" pass
that annotates the AST with every disposal site is the refactor that makes
any second backend a printer, and it is the one piece of this note that pays
for itself without a scripting tier.

Recipes for the counts quoted in the findings:

```
grep -c "ownership::" src/codegen/*.cpp src/codegen/*.hpp          # every file: 0
grep -c "landingpad\|personality\|__cxa_\|setjmp\|longjmp\|CreateInvoke" \
    src/codegen/*.cpp                                                 # every file: 0
grep -cE '^[A-Za-z_][A-Za-z0-9_ \*]*\bbz_[a-z0-9_]+\(' \
    src/runtime/bezel_runtime.h                                       # 471
grep -ci "state machine\|statemachine" src/codegen/*.cpp src/codegen/*.hpp
                                                                      # sums to 173
grep -rc "BEZEL_TEST(" tests/ --include=*.cpp                         # sums to 24,972
```

## 5. Backend options for a direct emitter

"Direct emitter" can mean three different things. All three keep the memory
model (§3) and the front end (§4); they differ in compile speed, dependencies,
debuggability and how much new code the project owns. All three presuppose
the seam and the hoisted release plan of §6 stages 1–2.

### 5.1 Option A — emit C, consume it two ways

The backend prints one C translation unit per program: a struct per class with
the vtable pointer at slot 0 and fields following (the layout
`LlvmBackend_Layout.cpp:2050` fixes today), a function per method, an explicit
pending-exception check and `goto` after each call, and calls into the
existing `bz_*` runtime for strings, collections, itable lookup and disposal.
Two consumers:

- **Run-from-source:** an in-process C compiler that compiles to memory and
  relocates, then the runner calls the emitted `main`. The obvious candidate
  is a Tiny-C-Compiler-class library; such compilers translate C at speeds an
  order of magnitude or more above `clang -O0` and need no linker (estimate
  from general knowledge; not measured here — §7 probe 3). The runtime
  archive is linked into the runner binary once, so the per-script cost is
  parse + check + C emission + in-memory C compile.
- **Release:** the same C file through `clang -O2` and the existing link,
  which is today's pipeline with the LLVM backend swapped for a text printer.

Costs, honestly: an in-process C compiler is a **new dependency**, which
`CLAUDE.md` classes as a project-level decision, and the well-known candidate
is LGPL, which a distributed runner must respect. C output puts a textual
seam in the middle of the compiler that must encode Bezel's overflow
semantics (§6: checked arithmetic on every operation), its 128-bit decimal
(`LlvmBackend_Layout.cpp:18-19`), and its `char` as a 32-bit code point
without letting C's own promotion rules leak through; the LLVM backend does
this with intrinsics, and a C backend does it with the compiler builtins the
consumer supports (`__builtin_add_overflow` and family — both candidate
consumers have them). Debug information comes free from the consumer.

### 5.2 Option B — emit machine code directly

A single-pass native emitter in the Wirth/Tiny-C tradition: one target at a
time, register allocation by convention (every local in a stack slot, a few
scratch registers), calls into the runtime by the System V ABI, code written
to a writable buffer then remapped executable. No third-party compiler in the
runner, fastest possible compile, and the project owns every byte.

Costs: two instruction sets to reach the machines the project already runs on
(x86-64 and AArch64), each with its own ABI corner cases for the 128-bit
decimal and for struct returns; no optimizer at all, so `-O2` release builds
still need LLVM or Option A; debug information must be emitted by hand (at
minimum line tables, or scripts cannot be stepped); and every backend bug is
a native crash rather than a C compile error. The existing codegen's size is
the honest estimate of how many distinct lowering cases exist; an emitter
that covers the tier's subset after the release-plan hoist is smaller, but
it is all new code with no oracle but the tests. And "no dependencies" holds
only for the runner: the compiler still uses LLVM's bitstream and object
libraries for artifacts (finding 3).

### 5.3 Option C — a bytecode interpreter

Emit a compact bytecode and interpret it. Simplest to write, trivially
portable, and the natural substrate for a REPL. The memory model is unaffected:
the interpreter executes the same disposal instructions the release plan
placed.

Costs: an order of magnitude or more slower than native for arithmetic-heavy
scripts (estimate); a second runtime-boundary convention for closures and
comparers the runtime calls back into; and no path to a release build without
one of the other options. It is the right choice only if the project decides
that scripting-tier performance does not matter, which the "compiled language"
premise argues against.

### 5.4 Cross-cutting decisions any option must make

- **Generics.** The backend monomorphizes every instantiation
  (`LlvmBackend_impl.hpp:2820`); the rationale's stated intent of sharing
  code for reference-type substitutions (rationale §10, line 1305) is not
  what ships. A tier with no user generics needs only the stdlib's
  instantiations, so a second backend can monomorphize too and lose nothing
  relative to today. Probe 4 in §7 bounds the set.
- **Exceptions.** Keep the thread-local-slot convention. It costs one compare
  and branch per call and needs nothing from the backend beyond a label per
  call site — precisely why it suits every option above.
- **Checked disposal.** Emit the member-entry flag test unconditionally in
  the tier (the spec's default since v0.307). The quarantine question (#4602)
  is the same for every backend, because the free is a runtime call.
- **Object layout.** The class layout exists and the runtime's itable lookup
  depends on it (vtable pointer at slot 0, no separate header word —
  `LlvmBackend_Layout.cpp:2050`); the array and dictionary headers are
  runtime-private (`bezel_runtime_internal.hpp:182`,
  `bezel_runtime_collections.cpp:449`). What is missing is a written,
  C-visible statement of the class layout that a second backend can target;
  producing it is probe 5's first half.

### 5.5 Recommendation

**Option A first, Option B only on evidence.** The C emitter delivers the
scripting experience (no link, in-memory compile, sub-second start) and the
release build from one backend, reuses the runtime unchanged, gets debug
information for free, and can be validated line-for-line against the LLVM
backend on the whole suite. Its one real cost is the dependency decision,
which is the owner's, and which §7's probe 3 prices before it is made. Option
B is the right *second* step if the in-memory C compile turns out to dominate
script start-up, at which point the C emitter's lowering decisions transfer
one-to-one into a native emitter and the C backend remains the oracle for it.
Option C is not recommended.

## 6. A staged path

Each stage is independently useful and stops cleanly.

0. **Measure the current pipeline** per phase for a representative
   1,000-line program: parse, sema, ownership, codegen, link, exec (§7,
   probes 1–2). Nothing below should be scheduled until this table exists.
1. **The backend seam.** Replace the direct `emitModule → llvm::Module` call
   in `Driver.cpp:611` with a backend interface selected by a driver option,
   with the LLVM backend as its first implementation. Pure refactor, no
   behaviour change, verified by the suite.
2. **Hoist the release plan.** Move the §28 discharge decisions that
   `LlvmBackend_Disposal.cpp` and `LlvmBackend_Assign.cpp` make into a
   backend-independent pass that annotates the AST (extending the twelve
   flags the ownership pass already sets, on the `OwnershipFacts` core), and
   make the LLVM backend consume the annotations. This is the stage most
   likely to find latent drift between the checker's and the backend's
   readings of §28 — which is a reason to do it, not a reason to skip it.
   Spec-derived tests first; the differential oracle is the pre-refactor
   backend.
3. **The subset gate.** A `--tier script` driver option that rejects the §2
   table's constructs with feature-naming diagnostics (D.1), plus the
   top-level-statement wrapper. Front end only; the LLVM backend still emits
   the code. This alone gives "Bezel scripts" that build.
4. **The C backend behind the seam**, run against the gtest suite and the
   rule corpus with the LLVM backend as the expected-value oracle.
5. **The runner.** A `bzs` entry that links the runtime, emits C to memory,
   compiles in-process and calls `main`; cache the compiled program keyed on
   source hash and compiler identity, on the model of the AOT bincache the
   test harness already uses.
6. **Optional native emitter** for one target, validated against the C
   backend the same way the C backend was validated against LLVM.

Stages 1–2 and 4–6 are implementation work of the kind `CLAUDE.md` describes
(spec-derived tests first); the top-level-statement wrapper and the "Core
minus named features" conformance claim are spec proposals and go through the
spec-change process before stage 3 lands.

## 7. What could not be verified here, and the probes that settle it

| Question | Probe |
|---|---|
| 1. Per-phase cost of today's pipeline on a 1,000-line program | Instrument the phases in `Driver.hpp:3-6` plus the link; the 0.12–0.26 s per test program is an envelope for small programs only |
| 2. How much of a script's start time is the front end at all | Same instrumentation on a 20-line program; if parse + sema + ownership are already tens of milliseconds, the backend is the whole story |
| 3. In-memory C compile time for the emitted program | Emit C by hand for one representative program and time the candidate in-process compiler on it; this is the number that decides between Options A and B, and it prices the dependency decision |
| 4. Which stdlib instantiations a corpus of scripts actually uses | Count generic instantiations across `tests/lit/` and the rule corpus; bounds the monomorphization set |
| 5. Whether the reuse half of the runtime links without the drop half, and what the C-visible class layout is | Build the nine "reuse" runtime files alone and resolve undefined symbols; diff `LlvmBackend_Layout.cpp`'s class layout against every `bz_*` entry that reads an object |
| 6. Whether the twelve AST flags plus `OwnershipFacts` are sufficient input for the release-plan pass | Enumerate every decision `LlvmBackend_Disposal.cpp` makes that reads neither; each is a fact the pass must compute |
| 7. The quarantine's growth under a long-running script | #4602's RSS probe, run under the tier's always-checked default |

## 8. What this note does not propose

It does not propose changing any normative rule. The two language-facing
items it identifies — top-level statements, and a "Core minus named features"
conformance claim — are proposals to be written, critiqued and ruled under the
spec-change process, and this note's §2 table is their draft scope, not their
text. It does not propose adopting an in-process C compiler; it prices that
decision (§5.1, §7 probe 3) for the owner. And it does not claim that a direct
native emitter is a bad idea — only that the tree's own conventions make a C
emitter the cheaper first step, that the release-plan hoist has to precede
either, and that the second step is easier for having taken the first.

## Sources

- `src/driver/Driver.hpp:3-6, 35-70`; `src/driver/Driver.cpp:34-35, 62-118, 265-298, 611`
- `src/common/OwnershipFacts.hpp:1-30`; `src/ast/Node.hpp:137-192`
- `src/codegen/LlvmBackend.hpp:37, 102`; `LlvmBackend_impl.hpp:2679-2697, 2820`;
  `LlvmBackend_Layout.cpp:12-29, 2050`
- `src/module/ModuleLoader.cpp:25-28`; `ModuleMetadata.cpp:27-28`; `src/sema/EntryPoint.cpp:110`
- `src/runtime/bezel_runtime.h:1796-1800, 2100-2101`; `bezel_runtime_concurrency.cpp:2722`;
  `bezel_runtime_refcounting.cpp:1-15`; `bezel_runtime_internal.hpp:182`;
  `bezel_runtime_collections.cpp:449`
- Spec v0.307: Appendix D (D.1–D.4), §15.4, §21.1, §22.10, §28.2, §28.3 (K1–K9, K4.3),
  §28.9, §28.13, §29.3–§29.8, §33; stdlib §4.21, §7.3
- Rationale v0.307: §5.5, §5.21, §5.28, §5.30, §5.31, §10 (line 1305)
- `docs/research/2026-08-10-dual-oracle-gate-investigation.md`;
  `docs/research/2026-08-19-gate-performance-audit.md` (anchor table; F15)
- Proposals 0005, 0103, 0127–0132; changelog v0.307; tracking issues #4601–#4605
