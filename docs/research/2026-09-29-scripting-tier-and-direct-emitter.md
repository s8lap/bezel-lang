# A scripting tier for Bezel without a garbage collector, and a direct-emit backend (2026-09-29)

*Research note. Non-normative. Written in a remote session with no LLVM and no
build tree: every number below is either counted from the tree (`wc`, `grep`,
stated with its recipe), quoted from a document or an in-tree comment (cited),
or marked as an estimate from general knowledge. Nothing was built or run.
Companion to the 2026-09-22 investigation that landed v0.307 (proposals
0127–0132), whose memory-model half this note takes as read.*

## Summary

The question: can a simplified Bezel become a scripting language — run from
source, no garbage collector — and can its compiler drop LLVM for a direct code
emitter? Six findings, each of which the tree supports rather than merely
permits:

1. **The no-GC property is a front-end property, not a backend one.** The
   pipeline is lex → parse → type-check → ownership-check → codegen
   (`src/driver/Driver.hpp:3-6`), and every release point — scope-end
   disposal, drop-on-overwrite, the K8 statement-end free, the owned-field
   cascade — is decided by the ownership checker before any code exists. A
   backend only has to emit the disposal calls the checker already placed.
   So a scripting tier keeps Bezel's memory management by keeping
   `src/ownership/` (41,583 lines) unchanged, and any backend, however
   simple, inherits "every heap object disposed exactly once".

2. **The spec already draws the inner boundary.** Appendix D defines a
   **Core** conformance level that excludes async, domains, channels, events,
   serialization synthesis, FFI, buffers/spans, contract proving, LINQ,
   `Shared<T>`/`Weak<T>` and move-only value types, and states that a Core
   implementation "satisfies the domain guarantees by construction" (D.4:
   one continuation, one domain). A scripting tier is Core minus a short
   list (§2 below), plus one syntax addition (top-level statements), plus the
   three v0.307 tools that make stored borrows cheap to get right (`scoped`
   on primary constructors, `OwningSlotMap<T>`, the trust-admission report).

3. **The backend is where the weight is.** `src/codegen/` is 184,321 lines —
   49% of the ~375k lines under `src/` — against 94k for sema, 42k for
   ownership, 8k for the parser and 0.9k for the lexer (recipe in §4). It is
   also the only place LLVM is used outside the test JIT harness, and it is
   the reason every program build ends in an external `clang` link
   (`src/driver/Driver.cpp:277-298`). Replacing it is a large job by line
   count; it is a *tractable* job because of finding 4.

4. **Bezel's codegen conventions are already the ones a naive emitter wants.**
   Exceptions do not use landing pads or a personality routine: a throw
   stores the exception in a global slot (`bz_exc`) and *returns*, and codegen
   emits the pending-check branch after every call
   (`src/runtime/bezel_runtime.h:2100-2101`, `:1796-1800`; one EH-related
   token in all of `src/codegen/`, recipe in §4). Dispatch goes through
   runtime vtables and itables (`bezel_runtime_itable.cpp`). Strings,
   closures and boxes are reference-counted by runtime calls
   (`bezel_runtime_refcounting.cpp:1`). Disposal is a runtime call plus a
   free. None of that needs an optimizer, SSA, or unwinding tables. The one
   genuinely hard lowering in the current backend is the iterator/async
   state machine (≈570 mentions across the codegen files); async is Extended
   and out, and synchronous iterator methods are the one Core feature this
   note recommends deferring from the scripting tier's first version.

5. **The runtime is reusable as-is.** It is 21,369 lines of C++ behind a C
   ABI of 455 `bz_*` entry points (`src/runtime/bezel_runtime.h`). The
   files a scripting tier needs — strings, refcounting, collections, file
   I/O, encoding, time, decimal/guid, itable — total ≈9.7k lines; the files it
   drops — concurrency, sync, events, serialization, shared/weak — total
   ≈9.1k and map one-to-one onto Appendix D's Extended features. A C-emitting
   or machine-emitting backend calls the same entry points the LLVM backend
   calls today.

6. **The existing suite is a differential oracle for a second backend.** The
   ~15.3k gtests run programs through `runAot` against the real driver, and
   the rule corpus (2,184 cases at v0.307) states verdicts from the
   documents. A second backend selected by a driver option can be run
   against both, with the LLVM backend's outputs as the expected values.
   This is the strongest argument for doing the work *inside* this
   repository rather than as a fresh project: a fresh project has no oracle.

The recommendation (§6) is a staged path: gate the subset first, then a
**C-emitting backend** consumed two ways — in-process by a small C compiler for
run-from-source, and by `clang -O2` for release builds — with a direct
machine-code emitter as a later specialization only if the C step proves to be
the bottleneck. The reasons, and the honest costs of each option, are in §5.

---

## 1. What "scripting language" has to mean here

Three properties, in the order a user notices them:

- **Run from source in one command, fast.** `bzs script.bzl args` should
  compile and start in well under a second for a thousand-line script,
  cold. Today's per-program figure is 0.12–0.26 s *warm* for the test
  programs, and that includes an LLVM pass pipeline and an external link
  (`docs/research/2026-08-19-gate-performance-audit.md`, anchor table; F15
  estimates the bare-`clang` link alone at ~100–300 ms when the system
  linker is bfd). The link step is the first thing a scripting path removes.
- **Small programs need no ceremony.** Bezel already delivers this for the
  common case: a class with no destructor, no owned non-trivial field and no
  owning inline array is trivial-dispose and is freed silently at its owner's
  release point (spec §28.9; rationale §5.28). `var sb := new StringBuilder();`
  needs nothing. What remains announced is disposal *with an effect* — a file
  closing — and that announcement is the feature, not the tax.
- **Deterministic memory without a collector.** Kept unchanged: K1–K9 of
  §28.3, checked disposal mode (the default since v0.307), and the
  runtime-managed tier (string, function values, boxes) whose reclamation
  the spec leaves to the implementation and the runtime implements with
  reference counts. A script's object graph is a tree of single owners with
  borrows, exactly as a service's is; the difference is only which features
  are reachable.

What a scripting language does **not** have to mean: dynamic typing. The
ownership classification of every expression is computed from its static type
(§28.3's producers table); a dynamically typed variant would have nothing to
classify. The scripting tier is statically typed with local inference (`var`),
which Bezel already has (§5.5: locals are inferred, boundaries are annotated).

## 2. The subset: Core minus a short list, plus one addition

Appendix D's Core is the starting point. The tier removes from Core:

| Removed from Core | Why | What a script uses instead |
|---|---|---|
| User-declared destructors (§28.4) | Every user class becomes trivial-dispose, so silence is honest by §28.9's own criterion (rationale §5.28 item 5: silence is unsound only where disposal has an effect). Resources stay stdlib classes with destructors, bound by `using var`. | `using var f := File.OpenRead(...)`; a user class holding a resource marks the field `owned`, becomes non-trivial by structure, and draws the must-use warning — the derived-ceremony rule unchanged |
| Iterator methods (`yield`, §29.3–§29.8) | The only Core lowering that needs a state machine (finding 4). `for` over collections, ranges and strings stays. | Collections, `Drain()`, ranges; a later version adds `yield` once the emitter is stable |
| Extension blocks (§26), operator overloading (§25), user generics (§14 declarations) | Each is a sema surface a script rarely needs and an emitter must specialise for. Stdlib generic *instantiations* (`Array<int>`, `Dictionary<string, T>`) stay — see §5.3 on monomorphization. | Plain methods; the stdlib collections |
| Contracts (§22.16) and `pure` (§22.15) | Prove-time features; nothing a script gains. | — |
| Multi-module programs (§2.8 artifacts) | A script is one compilation unit plus the stdlib prelude. | `import` of stdlib modules only, in version 1 |

Everything else in Core stays: nullability, records, enums, choices with `case`,
classes, single inheritance and interfaces, properties and indexers, lambdas,
exceptions, ranges, string interpolation, and the whole of §28 minus the
carve-outs above. From the Extended list the tier *adds back* exactly one
feature: **Shared ownership** (`Shared<T>`/`Weak<T>`, §28.13), because it is
the recorded escape hatch for the residue single ownership cannot express
(two long-lived owners; observer graphs), and rationale §5.21 already declined
every implicit alternative. With `OwningSlotMap<T>` (stdlib §4.21, v0.307) for
graphs with deletion and `scoped` on primary-constructor parameters (§22.10,
v0.307) for the checked back-reference, the three tools the 2026-09-22
investigation named are all present in the tier.

**The one addition: top-level statements.** §33 requires a `Main` method on a
class, resolved by `--main-class`. A script wants `Console.WriteLine("hi");` at
file scope. The desugaring is mechanical and needs no new semantics: a file
whose top level contains statements is wrapped in an implicit module and an
implicit class whose `Main(args: Array<string>)` body is the statement list, in
order, with top-level declarations hoisted beside it. Ownership rules apply to
that body as to any method body — a top-level `using var` is disposed at end of
script — which is the behaviour a user expects. This is a design change under
spec-change-process §2 and would be its own proposal; nothing else in this note
changes the language.

**Feature gating is Appendix D's rejection discipline, not a fork.** D.1
requires an implementation that omits a feature to *reject* its use "with a
diagnostic naming the construct and the feature". A `--tier script` driver
option that rejects the constructs in the table above, by name, keeps one
grammar, one sema, one ownership checker and one spec. The scripting tier is a
conformance claim of the "Core minus named features" shape D.1 already allows
for "Core plus named features"; that asymmetry (minus, not plus) is the second
thing a proposal would have to say.

## 3. What survives of the memory model, precisely

Every rule of §28 that is Core survives unchanged; this section lists what the
tier's removals *simplify*, so the reader can see that nothing load-bearing is
lost.

- **Teardown order becomes unobservable.** With no user destructors, the
  cascade over `owned` fields runs no user code, so sibling borrows under one
  owner cannot observe a half-torn-down parent. The rules stay (they are
  cheap and the stdlib's destructor-bearing classes still exist), but the
  class of bugs they guard against cannot arise in user code.
- **The residual-trust admissions stay, and stay backstopped.** A parameter-
  sourced borrow stored in a plain field is still §28.3's trust clause with
  checked mode as its backstop. For scripts checked mode is simply always
  on: the retention cost recorded in rationale §5.30 (the reference
  compiler keeps every disposed block) is bounded by a script's lifetime,
  and the `--unchecked-disposal` release relaxation exists for the program
  that outgrows the tier. The trust-admission report (`--report-trust-admissions`,
  §28.3) is the tool that lists what a script is trusting.
- **Domains vanish.** D.4 already states it: no `async`, one continuation,
  one domain, and the domain clause of K4.3 — the one admission with no
  runtime backstop at all — cannot arise.
- **The K8 boundary and the store lint stay.** `groups.Add("odds", [1, 3, 5])`
  still dies at statement end and still warns (§15.4). Scripts hit this
  shape often; the warning's remedy (bind the value first, or use the owning
  family) is one line.

What the tier gives up is exactly what the 2026-09-22 discussion of a
"trivial-dispose-only Bezel" found: graphs with deletion go through handles,
shared caches through `Shared<T>`, and long-lived collections of borrows do not
exist. Those are the model's limits, not the tier's.

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
| `src/ownership/` | 41,583 | reuse unchanged | the no-GC property lives here |
| `src/flow/`, `src/ast/`, `src/common/`, `src/support/` | 9,853 | reuse | — |
| `src/module/` | 13,000 | reuse, mostly idle | single-unit compilation still loads the prelude |
| `src/driver/` | 1,247 | extend | a backend switch and a run-from-source entry |
| `src/codegen/` (LLVM) | 184,321 | **replaced for the tier** | kept for release builds and as the differential oracle |
| `src/runtime/` | 21,369 | reuse ≈9.7k, drop ≈9.1k | see the split in finding 5; the C ABI is the contract |

Two observations follow. First, the reused front end is roughly 170k lines
that already exist and are tested; the tier writes none of it. Second, the
replaced backend is the single largest component, but its *responsibilities*
are narrower than its size suggests, because so much lives in the runtime:
layout of built-in types (`LlvmBackend_Layout.cpp:12-29` — integer widths,
decimal as four 32-bit words, string and `Object` as heap pointers), calls,
disposal emission, assignment with drop-on-overwrite, and the state machines.
A backend for the tier implements the first four and defers the fifth.

The EH-token recipe behind finding 4, for the reader who wants to check it:

```
grep -c "landingpad\|personality\|__cxa_\|setjmp\|longjmp\|CreateInvoke" src/codegen/*.cpp
```

returns a single hit across the directory, and the runtime header's own
description of `throw` is "stores … in the global exception slot (bz_exc) and
returns (codegen emits the unwind branch after the call)".

## 5. Backend options for a direct emitter

"Direct emitter" can mean three different things. All three keep the memory
model (§3) and the front end (§4); they differ in compile speed, dependencies,
debuggability and how much new code the project owns.

### 5.1 Option A — emit C, consume it two ways

The backend prints one C translation unit per program: a struct per class
(header word, vtable pointer, fields), a function per method, explicit
`if (bz_exc_pending()) goto unwind_N;` after each call, and calls into the
existing `bz_*` runtime for strings, collections, dispatch and disposal. Two
consumers:

- **Run-from-source:** an in-process C compiler that compiles to memory and
  relocates, then the runner calls the emitted `main`. The obvious candidate
  is a Tiny-C-Compiler-class library; such compilers translate C at speeds an
  order of magnitude or more above `clang -O0` and need no linker (estimate
  from general knowledge; not measured here). The runtime archive is linked
  into the runner binary once, so the per-script cost is parse + check + C
  emission + in-memory C compile.
- **Release:** the same C file through `clang -O2` and the existing link,
  which is today's pipeline with the LLVM backend swapped for a text printer.

Costs, honestly: an in-process C compiler is a **new dependency**, which
`CLAUDE.md` classes as a project-level decision, and the well-known candidate
is LGPL, which a distributed runner must respect. C output puts a textual
seam in the middle of the compiler that must encode Bezel's overflow
semantics (§6: checked arithmetic on every operation), its 128-bit decimal,
and its `char` as a 32-bit code point without letting C's own promotion rules
leak through; the LLVM backend already does this with intrinsics, and the C
backend does it with the compiler builtins the consumer supports
(`__builtin_add_overflow` and family — both candidate consumers have them).
Debug information comes free from the consumer.

### 5.2 Option B — emit machine code directly

A single-pass native emitter in the Wirth/Tiny-C tradition: one target at a
time, register allocation by convention (every local in a stack slot, a few
scratch registers), calls into the runtime by the System V ABI, code written
to a writable buffer then remapped executable. Zero dependencies, fastest
possible compile, and the project owns every byte.

Costs: two instruction sets to reach the machines the project already runs on
(x86-64 and AArch64), each with its own ABI corner cases for the 128-bit
decimal and for struct returns; no optimizer at all, so `-O2` release builds
still need LLVM or Option A; debug information must be emitted by hand (at
minimum line tables, or scripts cannot be stepped); and every backend bug is
a native crash rather than a C compile error. The existing codegen's size
(184k lines) is the honest estimate of how many distinct lowering cases
exist; an emitter that covers the tier's subset is smaller, but not by an
order of magnitude, and it is all new code with no oracle but the tests.

### 5.3 Option C — a bytecode interpreter

Emit a compact bytecode and interpret it. Simplest to write, trivially
portable, and the natural substrate for a REPL. The memory model is unaffected:
the interpreter executes the same disposal instructions the checker placed.

Costs: an order of magnitude or more slower than native for arithmetic-heavy
scripts (estimate); a second runtime-boundary convention for closures and
comparers the runtime calls back into; and no path to a release build without
one of the other options. It is the right choice only if the project decides
that scripting-tier performance does not matter, which the "compiled language"
premise argues against.

### 5.4 Cross-cutting decisions any option must make

- **Generics.** The LLVM backend specialises value-type instantiations and
  shares code for reference-type ones (rationale §14, line 1305: "specializes
  for value-type substitutions … and shares for reference-type
  substitutions"). A tier with no user generics needs only the stdlib's
  instantiations; the simplest emitter monomorphises every one it meets.
  Code size is irrelevant for scripts. Sharing can be added to the release
  path later without touching the tier.
- **The prelude.** The driver parses the auto-imported stdlib prelude sources
  on every compile (`src/driver/Driver.cpp:103-118`). A run-from-source
  path should snapshot the checked prelude once per compiler build and load
  it, or the prelude parse becomes the floor of every script's start time.
  This is measurable before any backend work (§7, probe 1).
- **Exceptions.** Keep the global-slot convention. It costs one compare and
  branch per call and needs nothing from the backend beyond a label per call
  site — precisely why it suits every option above.
- **Checked disposal.** Emit the member-entry flag test unconditionally in
  the tier (it is the default since v0.307). The quarantine question (#4602)
  is the same for every backend, because the free is a runtime call.
- **Object layout.** Freeze a C-visible layout (header, vtable pointer, fields
  in declaration order) so Option A's structs, Option B's offsets and the
  runtime agree. The runtime already fixes the array and dictionary headers
  (`bezel_runtime.h:122-128`, `:261`); classes need the same treatment
  written down.

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
   1,000-line program: prelude parse, user parse, sema, ownership, codegen,
   link, exec. Nothing below should be scheduled until this table exists
   (§7, probes 1–2).
1. **The subset gate.** A `--tier script` driver option that rejects the §2
   table's constructs with feature-naming diagnostics (D.1), plus the
   top-level-statement wrapper. Front end only; the LLVM backend still emits
   the code. This alone gives "Bezel scripts" that build today.
2. **The C backend behind the codegen interface**, selected by a driver
   option, run against the gtest suite and the rule corpus with the LLVM
   backend as the expected-value oracle. Differential failures are backend
   bugs by construction; the corpus's spec-stated verdicts catch the cases
   where both backends are wrong together.
3. **The runner.** A `bzs` entry that links the runtime, snapshots the
   prelude, emits C to memory, compiles in-process and calls `main`. Cache
   the compiled program keyed on source hash and compiler identity, on the
   model of the AOT bincache the test harness already uses.
4. **Optional native emitter** for one target, validated against the C
   backend the same way the C backend was validated against LLVM.

Stages 1–2 are implementation work of the kind `CLAUDE.md` describes
(spec-derived tests first); the top-level-statement wrapper and the "Core minus
named features" conformance claim are spec proposals and go through the
spec-change process before stage 1 lands.

## 7. What could not be verified here, and the probes that settle it

| Question | Probe |
|---|---|
| 1. How much of a script's start time is the prelude parse? | Time `Driver::compileFile` with and without the prelude prepend on a 20-line program; the prelude sources are constants (`Driver.cpp:103-118`), so a snapshot is a pure win if this is more than a few tens of milliseconds |
| 2. Per-phase cost of today's pipeline on a 1,000-line program | Instrument the five phases in `Driver.hpp:3-6` plus the link; the gate audit's 0.12–0.26 s per test program is an upper envelope for small programs only |
| 3. In-memory C compile time for the emitted program | Emit C by hand for one representative program and time the candidate in-process compiler on it; this is the number that decides between Options A and B |
| 4. Which stdlib instantiations a corpus of scripts actually uses | Count generic instantiations across `tests/lit/` and the rule corpus; bounds the monomorphisation set |
| 5. Whether the C-visible class layout matches what the runtime assumes | Diff `LlvmBackend_Layout.cpp`'s class layout against every `bz_*` entry that reads a class header |
| 6. The quarantine's growth under a long-running script | #4602's RSS probe, run under the tier's always-checked default |

## 8. What this note does not propose

It does not propose changing any normative rule. The two language-facing
items it identifies — top-level statements, and a "Core minus named features"
conformance claim — are proposals to be written, critiqued and ruled under the
spec-change process, and this note's §2 table is their draft scope, not their
text. It does not propose adopting an in-process C compiler; it prices that
decision (§5.1, §7 probe 3) for the owner. And it does not claim that a direct
native emitter is a bad idea — only that the tree's own conventions make a C
emitter the cheaper first step, and that the second step is easier for having
taken the first.

## Sources

- `src/driver/Driver.hpp:3-6, 35-70`; `src/driver/Driver.cpp:103-118, 265-298`
- `src/runtime/bezel_runtime.h:1796-1800, 2100-2101, 122-128, 261`;
  `src/runtime/bezel_runtime_refcounting.cpp:1-15`
- `src/codegen/LlvmBackend_Layout.cpp:12-29`; `src/codegen/LlvmBackend.cpp:10, 105`
- Spec v0.307: Appendix D (D.1–D.4), §28.2, §28.3 (K1–K9, K4.3), §28.9, §28.13,
  §29.3, §33, §22.10, stdlib §4.21
- Rationale v0.307: §5.1, §5.5, §5.21, §5.28, §5.30, §5.31, §14 (line 1305)
- `docs/research/2026-08-19-gate-performance-audit.md` (anchor table; F15)
- `docs/proposals/0005-selfhost-stdlib-collections.md` (the runtime/prelude
  boundary), `0128-checked-disposal-default.md`, `0130-owning-slot-map.md`,
  `0131-report-trust-admissions.md`
- Tracking issues #4601–#4606
