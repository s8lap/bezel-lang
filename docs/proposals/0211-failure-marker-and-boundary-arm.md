# Proposal 0211 — Failures and defects: a marker for the failure band, and the boundary catch arm

| | |
|---|---|
| **Status** | under-review — draft (2026-11-10), written on owner direction; not yet through an adversarial critique; it amends no normative document |
| **Tracking issue** | to be filed by the coordinator |
| **Target documents** | **spec** — §2.3 (one table row), §31.1 (one sentence), §31.2.1 (the catch-binding rule), §31.2.3 (one warning), §31.2.4 (one example arm), §31.2.5 (one sentence), §31.4 (the marker; one tree line), §30.14 (*Faults*; the combinators' fault sentence), Appendix C.7 (two bullets), Appendix E (one row). **stdlib** — §1 (the failure-signal rule: bands 1 and 4, one new paragraph), §2.3, §2.5, §2.14, §2.15, §2.17, §2.18, §6.1, §6.8, §6.9, §6.12, §7.5, §9.1–§9.3. **rationale** — a new §11.12; one sentence each in §11.4 and §12.6. **Guide 2**; one sentence each in Guides 4 and 6 and the Tour. **code-generation reference** — §6, §7.9, the §5 index. **proposals** — `README.md` (this row), `DECLINED.md` (the rows of §9.7, at graduation). No grammar change and no new keyword. |
| **Written against** | spec, stdlib and rationale **v0.355** (tree `a77e4718a`); probes on compiler release **r40** (`bezelc 0.0.0-a30bb11d`) |
| **Origin** | The research note [`docs/research/error-model/README.md`](../research/error-model/README.md) ("the note"), stages 0–1 of its §8.5. Owner direction, 2026-11-10: "Proceed with Fable research and proposals for fault/failure markers." |
| **Change class** | Design change ([`spec-change-process.md`](spec-change-process.md) §2). §9.5 gives the class of each part. Nothing here is a ratchet. |

*Conventions. An unprefixed § is a language-spec section at v0.355; stdlib, rationale, guide and note sections are marked. Quotations are verbatim. ▲ marks a name this proposal introduces. Evidence labels: **MEASURED** — produced by a script or probe under [`docs/research/error-model/probes-0211/`](../research/error-model/probes-0211/) and re-runnable (§12); **QUOTED** — text of a repository document, cited; **HYPOTHESIS** — a judgement or prediction. Probe results describe r40, which implements an older revision than the spec by design; they are informational.*

## 0. Summary

Bezel raises exceptions for two different things: a bug in the program, and a problem a correct program can meet (a missing file, malformed input, a peer that went away). The standard library's failure-signal rule already separates the two when it classifies a condition, but nothing a `catch` arm can name carries the difference, so the arm written at a boundary is `Exception`, which takes both. This proposal makes the difference a fact about the exception's class.

- **The marker.** A built-in interface ▲`IFailure`, with no members. A class that implements it is a *failure*; every other exception is a *defect* report; `OperationCanceledException` stays apart as cancellation. Only `Exception`-derived classes may implement it (§3).
- **The boundary arm.** `IFailure => …` is an ordinary interface arm, which §31.2.1 already admits. One rule changes: a binding arm over an interface binds the exception as an `Exception`, so `var f: IFailure => Log(f.Message)` works (§5.2).
- **The taxonomy.** Every exception type and every condition the standard library names is classified: 12 failure types, 17 defect types, cancellation, one fatal type; 115 conditions in 54 sections (§4, §11). Two classes are added so that the conditions raised today with a defect type can be failures without breaking an arm: ▲`ParseRangeException: OverflowException, IFailure` for out-of-range `Parse` input, and ▲`DomainFailedException: DomainFaultedException, IFailure` for a failure that leaves a spawned domain.
- **The catch-all.** An `Exception` arm keeps its meaning. One that handles everything in a single body, with no `IFailure` arm before it and no re-raise, draws a warning. The warning is not scheduled to become an error (§5.4).
- **Nothing else moves.** Ownership on the exception edge, task faults, supersession, purity and the FFI abort are the same for both kinds (§6). No recorded decline is reversed; one recorded band label is reworded and §8 says which. The call-site `(try E)` form is not proposed and nothing here depends on it (§7).

Measured reach on the four program corpora: no catch arm changes which exceptions it takes; one rule-corpus case changes verdict; 566 test arms would draw the new warning, none of them in a case that asserts silence (§9.1).

## 1. Scope and words

### 1.1 Scope

In scope: the marker, the catch-arm rule it needs, the classification of every stdlib exception type and condition, the domain join, the catch-all, and the guide and reference text. Out of scope: the `(try E)` atom of the note's §8.1 item 3 (it stays behind the note's §8.5 gate), retiring any `Try…` or `…OrNull` member, a `fails` clause, and any change to what a `Result` is for.

### 1.2 Words

The owner's direction and the note say *fault* for a bug and *failure* for an expected problem. The proposed normative text says **defect** for the first, for one reason.

- MEASURED: the spec uses a *fault* word (fault, faults, faulted, faulting) 213 times and the stdlib 117 times, and in nearly all of them it means "an exception was raised", whatever the cause: "the task faults with", `IsFaulted`, `DomainFaultedException`, "a defined fault", "the strict-fault model". The failure-signal rule opens "Every fault **condition** a member of this library declares" and calls the band this proposal marks "Boundary-consumed fault" (QUOTED, stdlib §1).
- QUOTED: the same rule already calls the bug band's exceptions "the residual defect exceptions" and says "A defect surface is never a recovery surface". Guide 2 says "defect exception".

So *fault* keeps its existing meaning (any raised exception; a task fault can be of either kind), *defect* is the bug band, and *failure* is the marked band. Reusing *fault* for the bug band would make the two meanings collide in about 330 places. Q1 asks the owner to confirm, or to order the sweep instead. In this document "bug" and "defect" mean the same thing.

## 2. The text today, and what it costs

**The classification exists, for conditions.** Stdlib §1 classifies "every fault condition a member of this library declares" into five bands. Band 1 is the caller's bug, reported by a `requires` clause or by "the residual defect exceptions (such as `Argument*` and `InvalidOperationException`)". Band 4 is the failure consumed at "a boundary, supervision, or retry site", reported by "a typed exception, producer-local" (QUOTED).

**A handler cannot name it.** "Catch dispatch is purely by type", and "a bare `Exception` arm catches every **catchable** throwable" (QUOTED, §31.1). A boundary that wants "every band-4 failure and no band-1 defect" has no type to write. It lists failure types by hand, or it writes `Exception`.

**What the corpora show** (MEASURED; `data/catch_arms.py`, `data/band_reach.py`; four corpora of test programs and document examples, about 36,900 programs):

| Fact | Count |
|---|---|
| catch arms, found by walking each `catch` block | 3,304 |
| arms naming `Exception` alone (the catch-all) | 623 |
| … of those, unguarded, with a body that neither re-raises nor is `()` | 566 |
| arms naming a failure type of §4 / a defect type / `OperationCanceledException` | 452 / 1,709 / 31 |
| exception class declarations; of those, deriving from a failure type | 1,187; 6 |

(The note counted 3,482 arms and 641 catch-alls with a regular expression that also matched some `case` arms nested in catch bodies; the walk is stricter.) These are test corpora, and most of the 566 arms are test oracles. The ergonomics study's 40 everyday programs are a second, small population: 6 of the 40 have a `catch`, with 8 arms, none of them a catch-all; four of the eight catch a class the program itself threw for an expected condition (`InvalidOperationException` twice, a user class twice), and under any type-keyed split those are defects until the class says otherwise (MEASURED, same script). Both populations are authored programs, not observed ones.

**Three types are raised in both bands** (MEASURED from the condition table of §11, and on r40 by probe f20):

- `OverflowException`: an arithmetic result outside its domain, and well-formed `Parse` input that spells a value the type cannot hold. One `OverflowException` arm takes both.
- `FormatException`: malformed input, and a format specifier the program wrote wrongly.
- `SerializationException`: malformed input on the read side, and three write-side conditions that are the program's own doing.

The note also listed `InvalidOperationException` and `ArgumentException` as straddling. Read condition by condition, the library raises neither for a failure: all 43 of their (section, type) pairs are band-1 conditions (§4.4).

**The band is lost at a domain join.** A failure and a bug raised in a spawned domain both arrive as `DomainFaultedException`, and no arm can tell them apart (MEASURED, probe f09; the note's p04).

## 3. The marker's shape

### 3.1 What the marker has to do

1. **Select.** One arm takes every failure and nothing else.
2. **Bind usefully.** A boundary arm logs, converts, retries, reports or re-raises (Guide 2 §7). Its binding must read `Message`, be passed where an `Exception` is expected, and chain as a cause.
3. **Cut across the class tree.** Two conditions are raised today with a class that existing arms name (`OverflowException` at `Parse`; `DomainFaultedException` at a join). Each must become a failure without leaving that class, or the arms stop matching.
4. **Be cheap to adopt and safe by default** for user exception classes.
5. **Bound a generic helper** (`Assert.Throws<TEx>`-style code).
6. **Add no ownership, sendability or FFI rule.**

### 3.2 The options

| | (A) plain interface, no members | (B) interface declaring `Message`, `StackTrace`, the chains | (C) base class `Failure: Exception` |
|---|---|---|---|
| 1. Select | yes: §31.2.1 admits interface arms (QUOTED; f05) | yes | yes |
| 2. Bind usefully, under today's rules | no: the binding has the interface's members only — `ToString()` and nothing else (f01, f04) | `Message` is readable; the binding is still not an `Exception`, so it cannot be passed to a logger or chained (f04). r40 also rejects the conformance for `Exception`'s own members ("a field does not satisfy a property contract", f03), though it accepts an inherited property of a user base class (f02), as §14.2 rule 1 says it should | yes, with no rule (f01) |
| 3. Cut across the tree | yes (f10; the note's p16) | yes | **no**: a class has one base |
| 4. User classes | `class E: Exception, IFailure` | the same | `class E: Failure`; a class under a defect base cannot join |
| 5. Generic bound | `where TEx: Exception, IFailure` (f07); the interface alone is rejected at the arm, as specified (f08) | the same | `where TEx: Failure` |
| 6. New rules elsewhere | none | none | none |
| Moving a type between bands later | add or remove an interface: only arms naming the marker change | the same | change the base class: every arm naming an ancestor changes; "base class changed" is **breaking** in Appendix E, where "An interface *added* to a type" is widening (QUOTED) |
| Existing arms that stop matching, if the two bridged conditions join the band | 0 | 0 | 26 `OverflowException` arms over a `Parse` call, in 13 files, plus the arms on `DomainFaultedException` whose child raises a failure (MEASURED, `band_reach.json`) |

Three more shapes were considered and are not in the table because they fail requirement 1 as a *type*: a virtual `IsFailure` property on `Exception` (an arm can test it only in a guard, so the unreachable-arm rule and generic arms cannot see it); a `failure` modifier on the class (a new keyword for what an interface already says); and a band chosen at the `throw` site (one class in two bands, which is the straddler problem made permanent).

### 3.3 Decision

**An interface, with no members, plus one rule at the catch arm.**

- ▲`IFailure` is a built-in interface declaring nothing (text: S11 in §10).
- **The binding rule** (S3–S5): an interface in an arm selects which exceptions the arm takes; it does not type the binding. A binding arm over an interface binds an `Exception`. This closes the gap in column (A), row 2, and it is not a new idea in the implementation: for a *multi-type* alternative that lists an interface, r40 already types the binding as `Exception` (f06), and a gtest pins it (`tests/sema/catch_multitype_no_common_base_v182_test.cpp`: "under v0.243 the binding types as `Exception`, which is what makes `e.Message` resolve", QUOTED). The spec's sentence for that case says "Every listed type is `Exception`-derived", which an interface is not; the rule states what the compiler does there and extends it to the single-interface binding.
- **Only an `Exception`-derived class may implement the marker** (S11). A record or a plain class that lists it can never be thrown, so the conformance is unobservable (f15 shows both compile today). The precedents are Appendix C.3's rule that records "may not implement `IDisposable`" (QUOTED; K1.7) and §31.2.1's rejection of a provably dead arm type.

Why not the base class, which needs no rule: requirement 3. The base class cannot hold the two bridged conditions, so either 26 arms and the domain arms flip, or out-of-range input stays a defect and a boundary arm lets "99999999999" through as a bug. It also makes every later re-banding a change of base class. Java drew its checked/unchecked line by base class and has `NumberFormatException` — the plainest input failure — under the unchecked `IllegalArgumentException` for exactly this reason (HYPOTHESIS: prior art from memory, no source consulted).

Why no members on the marker: with the binding an `Exception`, a member on the marker is a second way to reach what the binding already has.

What the binding rule costs: a binding arm over an interface no longer reaches the interface's own members through the binding. The spelling for that is the guard form, `var ex: Exception when ex is IRetryable => …`, whose arm sees `ex` narrowed to the interface (§12.7; the note's p18). MEASURED: the corpora have 5 interface-typed arms, 3 of them binding; one, rule case `s31_2_1_0007`, calls an interface member through the binding and changes verdict (§9.4).

### 3.4 Sendability, FFI, purity

- **Sendability.** No exception class is sendable (§30.14), and implementing the marker changes nothing: a `sealed` class implementing it is still rejected as a spawn argument (f17). The two decline rows on the point stand (§8).
- **FFI.** Exceptions do not cross the boundary; one that escapes a callback aborts the program (§32.8, QUOTED in §6.5). The kind does not change that.
- **Purity and trivial disposal.** An interface with no members adds no field and no destructor, so every built-in stays trivial-dispose and the pure-throw carve-out (§22.15) is untouched. ▲`ParseRangeException` declares `pure` constructors so that `Parse`, which is `pure`, may raise it (S9, L7).

## 4. The taxonomy

### 4.1 The test

**A condition is a failure when a correct program can meet it.** The environment, input from outside the program, a peer, or a time or capacity limit produced it. Otherwise it is a defect: the program is wrong. This is the line stdlib §1 already draws between band 1 ("something a correct caller already guarantees") and band 4; the proposal makes band 4's exceptions the marked ones.

Two consequences are stated because they are the ones a reader will question.

- **The throwing form of a query is a defect.** A missing key, an empty collection, an index out of range: the caller could not always know, and for that reason each has a query beside it (`GetOrNull`, `TryPop`, `FirstOrNull`, `TryGetValue`). Stdlib §1 says the throwing form "treats absence as a bug" (QUOTED). So `dict[key]` on an absent key passes an `IFailure` arm.
- **Arithmetic outside its domain is a defect**, in the language (`a * b`) and where a library member surfaces it (`Sum`, `TimeSpan` arithmetic, `DateTime.Add*`). A correct program keeps its arithmetic in range. This touches one recorded label; see §4.4 item 5.

### 4.2 The types

Thirty-one types after the proposal: **12 failures, 17 defect reports, cancellation, one fatal type.**

| Kind | Types |
|---|---|
| **failure** (implements `IFailure`) | built-in: `FormatException`. stdlib: `IOException`, `FileNotFoundException`, `DirectoryNotFoundException`, `EndOfStreamException`, `PathTooLongException` (§7.5); `SerializationException` (§2.15); `TimeoutException` (§6.1); `ChannelClosedException`, `ChannelFullException` (§6.9). new: ▲`ParseRangeException` (§9.1), ▲`DomainFailedException` (§6.8) |
| **defect** | built-in: `Exception` itself, `ContractViolationException`, `ArgumentException`, `ArgumentNullException`, `ArithmeticException`, `OverflowException`, `DivideByZeroException`, `InvalidOperationException`, `DisposedAccessException`, `NotImplementedException`, `NullReferenceException`, `IndexOutOfRangeException`, `MatchFailureException`, `TypeInitializationException`. stdlib: `ArgumentOutOfRangeException` (§2.5), `KeyNotFoundException` (§4.8), `DomainFaultedException` (§6.8; raised for a child's defect or cancellation) |
| **cancellation** | `OperationCanceledException` |
| **fatal** | `StackOverflowException` (no arm matches it, §31.4) |

Notes on four of them.

- `Exception` thrown as itself is a defect: `throw new Exception("…")` says nothing about the condition, and unmarked means defect (§4.5).
- `TypeInitializationException` is a defect even when the initializer's own exception was a failure. The cached instance is re-raised on every later touch of the type (§31.4), so nothing a handler does recovers it.
- `ChannelClosedException` and `ChannelFullException` are failures: the peer closed, or the buffer is full under load. Neither is something the sending program controls.
- `TimeoutException` has no producer in the landed library (stdlib §6.1, QUOTED: "provided for user libraries and future modules"); it is classified for those.

### 4.3 The conditions

`data/taxonomy.tsv` has one row for each (section, exception type) the standard library names, 134 rows; `data/taxonomy_check.py` fails if the document names a pair the table lacks (MEASURED: 268 mentions, 130 pairs, 57 sections, 25 types, all covered). The 115 rows where a member raises are §11. In summary:

| | Rows |
|---|---|
| defect conditions | 82 |
| failure conditions | 25 |
| cancellation | 6 |
| a domain join (kind carried from the original) | 2 |
| **change needed** | |
| failure condition raised today with a defect class → ▲`ParseRangeException` (stdlib §2.3, §9.1, §9.2, §9.3) | 4 |
| join → ▲`DomainFailedException` when the original was a failure (stdlib §6.8, §6.12) | 2 |
| defect condition raised with a failure class → recorded misfit (stdlib §2.6, §2.14, §2.15, §2.18, §3.1) | 5 |
| band label reworded, type unchanged (stdlib §2.5) | 1 |

The note priced the straddlers at "48 type-and-section pairs" for three types; this extraction, which also reads code comments, finds 52 for the same three (`ArgumentException` 21, `InvalidOperationException` 22, `OverflowException` 9), and all 52 are read and ruled in the table.

### 4.4 The straddlers, ruled

1. **Out-of-range input to `Parse` and `Convert.To*`** (`OverflowException` today; stdlib §2.3, §9.1–§9.3): a failure. The members raise ▲`ParseRangeException`, which derives from `OverflowException` and implements the marker (L5–L10). An `OverflowException` arm around a `Parse` call matches as before; an `IFailure` arm now takes bad input and still passes an arithmetic overflow. `TryParse` and `ParseOrNull` are defined as "exactly where `Parse` raises" and are unchanged.
2. **A failure leaving a spawned domain** (`DomainFaultedException` today): the join raises ▲`DomainFailedException`, which derives from `DomainFaultedException` and implements the marker (§6.3).
3. **An unrecognized format specifier** (`FormatException`; stdlib §2.18, §2.6, §3.1): a defect — the specifier is program text — raised with a failure class. `FormatException` must be a failure, because bad input is what it is mostly raised for and what a user will throw it for. The specifier condition is flagged as a **recorded misfit** under stdlib §1's own sentence ("a landed spelling the rule would place in a different band is a recorded misfit — flagged where it stands, moved only by its own graduation ruling", QUOTED) and not moved here (L19). Moving it to `ArgumentException` would flip at least 36 arms in 9 test files (MEASURED). Q7.
4. **Write-side serialization conditions** (`SerializationException`; stdlib §2.14, §2.15): an unregistered run-time type, a `Weak` reference to a target never strongly reached, a non-finite float on the JSON write path. Defects raised with a failure class; stdlib §2.15 already flags the third as a misfit. Flagged, not moved (L20).
5. **`DateTime.Add*`, `Subtract(span)`, `FromTicks` out of range** (`OverflowException`; stdlib §2.5). The type is right and stays. The section calls the condition "a band-4 catchable fault, not a band-1 argument defect" (QUOTED; the v0.292 ruling). With band 4 now meaning "raised with a failure class", that label would say an arithmetic overflow is a failure, which it is not. L4 rewords it to "a band-1 defect of the arithmetic domain … raised with the arithmetic class and not with an argument-blaming one". The v0.292 decision — not `ArgumentOutOfRangeException`, because the blame is not the argument's — is kept in full; only the band word moves (§8).
6. **Every `InvalidOperationException` and `ArgumentException` condition in the library** (43 pairs): a defect. State misuse, a second enumeration, a duplicate key at `Add`, an empty collection at `Pop`, a foreign handle. No change.

**The library's rule about its own shape** (L3): one class is of one kind; a member does not raise a failure with a defect class or the reverse; no failure class derives from a defect class, except the two bridging classes of items 1 and 2, each recorded with its reason. The exception is deliberate. Stdlib §4.8 keeps `KeyNotFoundException` out from under `InvalidOperationException` "so that a state-fault catch arm does not silently swallow missing-key lookups" (QUOTED). That decline was about a derivation that would make arms *newly* take a type. The two bridges keep arms taking what they take today.

### 4.5 User exception classes

A user class is a defect until it lists the marker: `class ConfigError(message: string): Exception(message), IFailure` (f19). MEASURED: of 1,187 exception class declarations in the corpora, 6 derive from a failure type and would be failures by inheritance; the rest are defects until they opt in. HYPOTHESIS: this is the right default. The other default lets a boundary arm swallow whatever a library author forgot to classify, which is the problem this proposal exists to remove. Q9.

### 4.6 "One mechanism per fault condition"

The no-dual rule is unchanged. What is added is a second, type-level statement: one *kind* per exception class, and the band fixes the kind (L1–L3, the revised failure-signal rule text in §10).

## 5. The boundary arm

### 5.1 Dispatch

No dispatch rule changes. `IFailure => …` is an interface arm; it "matches exactly the throwables whose class implements the interface" (QUOTED, §31.2.1). MEASURED on r40 with a program-declared marker (f13): the arm takes a marked class and a marked subclass of `OverflowException`, and passes a contract violation, an index fault, a null fault and a cancellation.

### 5.2 The binding

`var f: IFailure => …` binds `f` as an `Exception` (S3–S5). In a binding alternative an interface counts as `Exception` when the nearest common base is computed, which is what r40 does (f06). A type parameter keeps its class bound: `where TEx: Exception, IFailure` (f07).

### 5.3 Ordering, guards, re-raise

All unchanged, and all measured on r40 (f05):

- Arms are tried in order; a class arm written before the marker arm wins for its class.
- A guard on a marker arm declines as any guard does.
- A bare re-raise from a marker arm keeps the exception's class. `throw f` with a marker-typed operand is rejected (f16), as §31.1 says of every interface-typed operand; under the binding rule `f` is an `Exception`, which §31.1 still rejects as a borrowed operand. The re-raise is `throw`.
- The unreachable-arm warning already covers the marker: an arm is unreachable when each type it names is "a class implementing an interface type the earlier arm names" (QUOTED, §31.2.3). After `IFailure => …`, an arm on `FormatException` is unreachable. (r40 emits no warning there, f05 case 4: a compiler gap that predates this proposal.)
- Deliberate suppression gains a narrower form: `IFailure => ()` suppresses failures and lets defects pass (S8; f19).

### 5.4 The catch-all

An `Exception` arm stays the catch-all, with the meaning §31.2.3 gives it. Defects are "catchable on purpose" (QUOTED, proposal 0076 §6): a test's oracle, a server that must keep serving, the logger of last resort. What the proposal targets is the arm that handles a failure and a bug with the same code because no other arm type existed.

**The rule (S6).** An unguarded arm naming `Exception` alone draws a warning unless (a) an earlier unguarded arm of the same block names `IFailure`, or (b) its body contains a bare re-raise, or (c) its body is `()`. To take both kinds in one body on purpose, list both: `var ex: IFailure | Exception => …` (f19 case 3).

- It is syntactic and local: one `catch` block's arm patterns and one arm body. It models no raise and no arm selection, so it does not touch the decline of type-keyed reachability (§8).
- A conversion (`Exception => throw new Wrapped(…)`) does not silence it. A wrapper class may be a failure, and converting everything into it would turn defects into failures.

**Reach** (MEASURED, `catch_arms_summary.json`): 566 arms in 234 files would draw it — 468 in gtest programs, 59 in rule cases, 33 in lit files, 6 in document examples. No rule case with `expect: quiet` contains one (the 55 affected cases are 37 `run` and 18 `reject`, which do not fail on a warning). 12 lit files contain one and also assert on the absence of diagnostics; they are the files to read when the compiler implements the warning (`data/undivided_lit_files.txt`). The 40 everyday programs draw it 0 times.

**Ramp.** The warning lands with the marker and stays a warning. This proposal schedules no promotion; one would be a design change with its own proposal. It is therefore not the first step of a ratchet, and the Migration note records it under *newly-warned* only.

**Is it worth a compiler rule?** HYPOTHESIS: yes. The habit it corrects is the default in the languages Bezel's readers and code generators come from, the remedy is one line, and the documents themselves teach the undivided arm in four places today (§9.2). The evidence is test-corpus counts and the documents' own examples, not observed programs, which is the class of evidence this project's gates usually decline to act on. Q8 gives the owner the guide-only alternative.

### 5.5 Idioms

The three shapes the guide teaches (text: G2 in §10; MEASURED to compile and run on r40 with a program-declared marker, f19):

```bezel
// 1. A boundary that answers failures and lets bugs propagate.
catch
  var f: IFailure => do return Response.BadGateway(f.Message); end
end;

// 2. A supervisor: failures answered, bugs reported, the loop keeps serving.
catch
  var f: IFailure => do return Response.BadGateway(f.Message); end,
  var ex: Exception => do ReportDefect(ex); return Response.InternalError(); end
end;

// 3. A failure class of your own.
public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;
```

(Shapes 1 and 2 read `f.Message` through a marker binding, which needs the binding rule; r40 rejects that read today, f04. `Response`, `ReportDefect` and the surrounding `try` belong to the example.)

## 6. Interactions

### 6.1 Ownership on the exception edge: unchanged

The kind is read by a catch arm's type test and by nothing else. In particular:

- **The throw.** "`throw x` is a source-visible kill — the keyword itself is the announcement ([K3.13])" and the operand's ownership "transfers to the runtime's exception machinery, which disposes the exception after the matching catch arm completes" (QUOTED, §31.1). Neither sentence mentions the operand's class beyond its being `Exception`-derived.
- **The edge.** "The exception edge performs the same implicit disposal as the normal edge, on the single establishment-ordered cleanup stack" (QUOTED, K3.15); §28.3's exception-path discharge derives from it. MEASURED (f12): a failure edge and a defect edge run `close b, finally, close a` identically.
- **A consumed argument.** "A raise from the consuming call does not hand the obligation back" (QUOTED, §28.8.1), for either kind.
- **Dispose-after-arm.** A failure that owns a payload is disposed after a bare marker arm with no binding (MEASURED, f12).
- **The caught borrow.** A binding typed `Exception` under §5.2 is the same borrow of the in-flight exception; §31.1's escape rules apply as written.

Kernel classification ([`spec-change-process.md`](spec-change-process.md) §9): no K rule, axiom or register row is touched. Body-locality (proposal 0105 §1) of the three new compile-time rules: the binding rule, the conformance restriction and the warning are each **local**.

### 6.2 Async

A task's fault is the exception instance; its class, and so its kind, rides it. MEASURED (f11): `await` in a `try` whose arm is the marker takes a failure and passes a bug; two awaiters of one faulted task both see the marker; the frame's `using var` is disposed on both edges.

- `IsFaulted` means "completed with an exception", of either kind. No member is added to tell them apart; the task's exception is reachable only by awaiting (§30.9), and the arm does the telling.
- `Task.WhenAll` "re-raises the exception of the first non-successful input in input order" (QUOTED, §30.10). So a failure in the first input is re-raised while a bug in the second stays observable only on that task (MEASURED, f11 case 4). This is today's behaviour and is not changed; §13 records it and Q10 asks whether the owner wants more.
- `TaskScope`, `WhenEach`, `FromException`, `SetException`: the kind is the class of the exception they carry. No text changes.

### 6.3 Domains

"An exception that propagates out of the entry does not cross the boundary as an object (no exception class is sendable …)" (QUOTED, §30.14). That stays. One fact about the original crosses with the two strings: whether its class implements the marker. The join raises ▲`DomainFailedException` when it does and `DomainFaultedException` when it does not (S13, L15–L17).

- A class's interfaces are fixed, so one class cannot be "a failure when the original was". Hence a subclass. MEASURED (f10): a user-declared class of that shape is taken by a marker arm and by an existing `DomainFaultedException` arm; the plain class is taken by the second only.
- Cancellation leaving an entry raises the base class, so a marker arm at a join never takes a cancelled child.
- The kind composes across nested spawns: a `DomainFailedException` that leaves an entry implements the marker, so the outer join raises `DomainFailedException` again.
- `Parallel.*` uses the same vocabulary (S14, L17).
- Proposal 0110 (under review) would add an `ExitKind` of `Fault` or `Cancelled` to `DomainFaultedException`. The two are orthogonal: 0110's property tells a fault from a cancellation; this class tells a failure from a defect among faults. If both land, `DomainFailedException` always has `ExitKind = Fault`.

### 6.4 Contracts and purity

`ContractViolationException` is a defect and never implements the marker; a clause "states a proposition" and its violation is "a bug report, not a result" (§22.16). MEASURED (f13): a `requires` violation passes a marker arm. `requires` remains wrong for input whose invalidity is a normal outcome; that input is a failure, raised by the body.

### 6.5 FFI

"Exceptions do not cross the boundary. An exception escaping a Bezel callback reaches the FFI boundary and **aborts the program**" (QUOTED, §32.8). The rule is the same for both kinds: native frames cannot be unwound for a failure any more than for a bug. A wrapper that turns `errno` into an exception (§32.9) raises a failure class, normally one of the `IOException` family. This could not be probed: on r40 a native callback segfaults even for rule case `s32_8_0021`'s own program (§13).

### 6.6 Supersession and cause chains

Later-wins is unchanged (§31.2; rationale §11.8). It has one consequence under the marker that must be stated: a `finally` or a disposer that raises a *failure* while a *defect* is propagating replaces the defect, and a marker arm then takes the cleanup failure (MEASURED, f14). The defect is not lost: it is the first entry of the failure's `SupersededChain` (§31.4) and `ToString()` renders it. A boundary that must not answer such a failure as an ordinary one tests the chain in a guard — `var f: IFailure when f.SupersededChain.Count = 0 => …` — and the superseding failure falls to the next arm. No rule is proposed. The reopening bar rationale §11.8 states for primary-wins ("arm matching on the primary is the one gain the snapshot does not provide", QUOTED) describes this case; the marker makes it more visible and supplies no field evidence, so the bar is not met.

Cause chaining copies strings; the wrapper's kind is its own class's. The guide's rule is: wrap a failure in a failure; do not wrap a defect, re-raise it. `ExceptionInfo` is not given a kind field: it "is a record of `string`s" (QUOTED, §31.2) whose widening was declined once already (§31.4), and the entry's `TypeName` identifies it in a log.

### 6.7 What does not change, in one list

`throw` as an expression; `finally` ordering; the pure-throw carve-out; `Task.FireAndForget`'s logging; the uncaught-exception report and exit code 70 (§33.3), for both kinds; `yield` restrictions around `try`; the absence of a `throws` clause (§31.3).

## 7. Compatibility with the deferred `(try E)`

The note defines the atom as: evaluate `E`; "If evaluation of `E` raises an exception whose class implements the marker, handling of that exception completes at the atom … and the atom's value is `null`. Any other exception propagates" (QUOTED, note §8.1). Everything it needs from this proposal is the predicate "the class implements `IFailure`", decidable at run time from the object. This proposal supplies that predicate and nothing more.

- The rulings of §4.4 are the ones the atom would inherit: `(try int.Parse(s))` would yield `null` for out-of-range text (item 1) and `(try list.First())` would propagate (item 6). No ruling here was bent toward the atom; each is justified by the boundary arm alone.
- Cancellation is not a failure, so the atom would never swallow it.
- ▲`DomainFailedException` makes `(try await join)` meaningful without further design.
- Nothing here is defined in terms of the atom, the warning's remedies do not mention it, and no text in §10 would need rewording if the atom never lands.

The atom's own gate (note §8.5) is untouched: this proposal provides none of its four conditions.

## 8. Recorded declines and gates

No recorded decline is reversed. The rows read, by their index name in `DECLINED.md`, and what this proposal does to each:

| Row | Effect |
|---|---|
| *Value-form try expression* (D-0005); proposal 0076 §7 ("Untouched and *reinforced* — on both grounds") | **Untouched.** No expression form is added. The marker arm is a `catch` arm at a boundary; the doctrine "catch at boundaries" (Guide 2 §7, from 0076 §6) is what it serves. Admission test 3 of proposal 0061 ("does not make a doctrine-declined path more ergonomic") is not engaged: no interior call site gets easier. |
| *`Result`-first error model*; proposal 0076 §6's four-condition bar | **Untouched.** None of the four conditions is claimed or needed. |
| *Information content as the failure-band test* | **Untouched.** The test of §4.1 is whether a correct program can meet the condition, not how much the exception carries. |
| *`ArgumentOutOfRangeException` as the `DateTime` … range fault* (stdlib §2.5, v0.292) | **Decision kept, one label reworded.** The row records the condition as "re-classified from the band-1 defect surface its `Argument*` label implied to band 4's catchable fault". The declined type stays declined and `OverflowException` stays. L4 moves the band word from 4 to 1 ("of the arithmetic domain"), because band 4 now fixes a failure class. New ground: the marker, which did not exist at v0.292, makes band 4 type-visible, and an arithmetic overflow must not be taken by a failure arm. The row's summary needs the same rewording at graduation (§9.7). |
| *`KeyNotFoundException` deriving from `InvalidOperationException`* | **Untouched; its reasoning is applied.** No failure class derives from a defect class except the two bridges, which preserve existing matching (§4.4). |
| *Checked enum conversion — a dedicated fault subclass rather than `OverflowException`* ("no plausible handler branches enum-cast-vs-overflow") | **Untouched.** ▲`ParseRangeException` is a subclass for a different condition, and it meets the row's own bar: a handler that branches on it exists, the boundary arm. |
| *`NotSupportedException`*; "the taxonomy stays deliberately small" | **Untouched.** Two classes are added, each because a handler selects on it. |
| *`Exception`-derived-throwable alternatives* (proposal 0080); *Non-`Exception` throwability* (retired) | **Built on.** Interface arms were kept admissible there on purpose; the binding rule extends §31.2.1's nearest-common-base rule. |
| *Modelling which `catch` arm a raise can reach by exception type* (proposal 0198 §7) | **Untouched.** The warning of §5.4 reads arm patterns, not raises. |
| *Primary-wins exception supersession* | **Untouched** (§6.6). |
| *Sendability requirement on throwable types*; *Stating `Exception`'s inherited state transfer-compatible* | **Untouched** (§3.4, §6.3). One bit crosses a join as the class of the exception the join raises. |
| *Per-site suppression spellings for the statically-certain-hazard lints* | **Not engaged.** The catch-all warning is not a certain hazard; like the must-use warning it has spellings that state intent (§5.4). |
| *Forcible domain teardown*; *Catchable allocation failure*; *`defer`/`errdefer`*; *`TextReader.Read()` returning `char?`*; *Enum name-parse tiers* | **Untouched.** |
| Rationale §17.4 (*Default interface methods*) | **Untouched.** The marker has no members. |
| Rationale §18 | Nothing deferred there is used. |

Proposal 0061 §2's operator gate is not engaged: no operator, no `Result` propagation.

## 9. Migration and conformance

### 9.1 What existing programs see

MEASURED on the four corpora (`data/band_reach.json`, `data/catch_arms_summary.json`), with the bridging classes of §4.4:

| Change | Sites |
|---|---|
| catch arms that take a different set of exceptions | **0** |
| `OverflowException` arms over a `Parse`/`Convert.To*` call (still match; a test that prints the *name* of the class raised would see `ParseRangeException`) | 26 arms, 13 files (`data/overflow_over_parse_files.txt`) |
| `DomainFaultedException` arms (still match) | 5 |
| interface-typed catch arms; binding ones; ones that reach an interface member through the binding | 5; 3; **1** (rule case `s31_2_1_0007`) |
| declarations or uses of the names `IFailure`, `ParseRangeException`, `DomainFailedException` | 0 |
| undivided catch-all arms (newly warned) | 566 arms, 234 files; 0 in a `quiet` rule case; 12 lit files that also assert no diagnostics |
| user exception classes that become failures without being edited | 6 of 1,187 |

Nothing is newly rejected except a type that lists the marker without deriving from `Exception`, and the corpora contain none.

### 9.2 Documents

**Guide 2** (text: G1–G5 in §10): the opening list; a new passage in §4 with the boundary arm and the user class; the §5 example's arm; two rows of the §7 table and one sentence after it. Its pin moves to the landing revision. It stands 27 revisions behind today (MEASURED, `check_docs.sh`), so the landing re-traces all of it.

**Other guides and the Tour**, one sentence each, at landing: Guide 4 §4 and §5, where a fault surfaces "as `DomainFaultedException`" (add: "or `DomainFailedException`, when the child's exception was a failure"); Guide 6 §2, where it says what `requires` is not for (add: "that input is a failure; a contract violation is a defect and passes an `IFailure` arm"); the Tour's §6, "**Exceptions** are for the unexpected" (add the marker in one clause).

**Document examples that would draw the warning**, to be rewritten in the same landing (MEASURED: 6 arms; 2 are in examples that are compile errors already): spec §31.1's closure example and §31.2.4's wrap example (S7 rewrites the second; the first keeps its meaning with `var ex: IFailure`), Guide 2 §5's `Handle` (G3), and stdlib §6.5's handler example (`var ex: Exception => Log(…)` becomes an `IFailure` arm).

**Code-generation reference** (C1–C2 in §10, plus index lines): §6's exception idiom lists the marker; §7.9 gains a pair on the catch-all; §5.2 gains `IFailure` and `ParseRangeException`, §5.6 `DomainFailedException`. Its pin moves in the same commit ([`spec-change-process.md`](spec-change-process.md) §9).

### 9.3 Conformance cases to add at landing

Each case states the verdict the proposed text requires. *Cites* names the edit of §10 whose sentence the case quotes.

| # | Section | Expect | Program | Cites |
|---|---|---|---|---|
| 1 | §31.4 | run | a user class listing the marker is thrown; an `IFailure` arm takes it | S11 "an `IFailure` arm takes every failure" |
| 2 | §31.4 | run | `InvalidOperationException` passes an `IFailure` arm and reaches an outer `Exception` arm | S11 "lets defects and cancellation pass" |
| 3 | §31.4 | run | `OperationCanceledException` passes an `IFailure` arm | S11 "a handler of failures never takes it" |
| 4 | §31.4 | run | `int.Parse("12x")` is taken by an `IFailure` arm | S11 "`FormatException` implements `IFailure`" |
| 5 | §31.4 | run | a class deriving from a marked class, not listing the marker, is taken | S11 "a kind is inherited and cannot be shed" |
| 6 | §31.4 | reject | a record lists `IFailure` | S11 "Only an `Exception`-derived class may conform" |
| 7 | §31.4 | reject | a class outside the `Exception` family lists `IFailure` | the same |
| 8 | §31.4 | reject | a record implements an interface that extends `IFailure` | S11 "the restriction then binds its implementers" |
| 9 | §31.4 | accept | an interface extends `IFailure`; an exception class implements it | the same |
| 10 | §31.2.1 | run | `var f: IFailure => Console.WriteLine(f.Message)` prints the message | S5 "binds `name` at `Exception`" |
| 11 | §31.2.1 | accept | `var f: IFailure => throw new Wrapped("x", f)` | S5 "chains as a cause" |
| 12 | §31.2.1 | run | `var ex: TimeoutException \| IFailure` reads `Message` | S4 "an interface counting as `Exception`" |
| 13 | §31.2.1 | reject | `var r: IRetryable => r.ShouldRetry()` | S5 "It does not type a binding" |
| 14 | §31.2.1 | run | `var ex: Exception when ex is IRetryable => ex.ShouldRetry()` | S5 "whose arm sees `ex` narrowed to the interface" |
| 15 | §31.2.1 | reject | an arm over `TEx` with `where TEx: IFailure` alone | S5 "never the interface alone" |
| 16 | §31.2.3 | warn | a sole `var ex: Exception => Log(ex)` | S6 "An undivided catch-all draws a warning" |
| 17 | §31.2.3 | warn | `Exception => throw new Wrapped("x")` (a conversion, no bare re-raise) | S6 "the arm's body contains a bare re-raise" |
| 18 | §31.2.3 | quiet | `IFailure => …` then `Exception => …` | S6 "an earlier unguarded arm … names `IFailure`" |
| 19 | §31.2.3 | quiet | `var ex: Exception => do Log(ex); throw; end` | S6, the second exemption |
| 20 | §31.2.3 | quiet | `Exception => ()` | S6, the third exemption |
| 21 | §31.2.3 | quiet | `var ex: IFailure \| Exception => Log(ex)` | S6 "lists both" |
| 22 | §31.2.3 | warn | `IFailure => …` then `FormatException => …` (unreachable arm; existing rule) | §31.2.3 "a class implementing an interface type the earlier arm names" |
| 23 | §31.2.5 | run | `IFailure => ()` suppresses a failure; a defect in the same block propagates | S8 |
| 24 | §30.14 | run | a child raises a marked class; an `IFailure` arm at the join takes it | S13 "the join raises `DomainFailedException`" |
| 25 | §30.14 | run | a child raises `InvalidOperationException`; an `IFailure` arm passes it, a `DomainFaultedException` arm takes it | S13 "An arm naming `DomainFaultedException` takes both" |
| 26 | §30.14 | run | a child raises a marked class; a `DomainFaultedException` arm takes it and reads the copied `Message` | the same |
| 27 | stdlib §6.12 | run | a `Parallel.Map` body raises a failure; an `IFailure` arm takes it at the `await` | L17 |
| 28 | stdlib §9.1 | run | `int.Parse("99999999999")` is taken by an `IFailure` arm | L7 "raises `ParseRangeException`" |
| 29 | stdlib §9.1 | run | an arithmetic overflow passes an `IFailure` arm | L7 "no arm on `IFailure` takes it" |
| 30 | stdlib §2.3, §9.2, §9.3 | run | `Convert.ToInt32`, `double.Parse`, `decimal.Parse` out of range: one case each, taken by an `IFailure` arm | L5, L9, L10 |
| 31 | stdlib §7.5 | run | opening a missing file is taken by an `IFailure` arm | L14 |
| 32 | stdlib §6.9 | run | `Send` after `Close`, and `Send` on a full bounded channel: one case each | L13 |
| 33 | stdlib §2.15 | run | malformed JSON is taken by an `IFailure` arm | L11 |
| 34 | stdlib §1 | run | `dict[missing]` and `xs.First()` on an empty array each pass an `IFailure` arm | L3 "the throwing form of a query is a defect and stays one" |
| 35 | §30.9 | run | a task faulted with a failure: an `IFailure` arm takes it at each of two awaits | S11 "a task's fault … the same for both kinds" |
| 36 | §31.2 | run | a `finally` raises a failure while a defect propagates; the `IFailure` arm takes it and `SupersededChain.Count` is 1 | §31.2 (unchanged), S11 |
| 37 | §22.16 | run | a `requires` violation passes an `IFailure` arm | S11 |

Cases 10–15 and 16–21 depend on rules the compiler does not implement; they enter the baseline as `fail` rows, which is the corpus's design.

### 9.4 Rule-corpus sweep (spec-change-process §5 item 3c)

One existing case states a verdict the proposal changes: `s31_2_1_0007` ("an interface-typed catch arm matches an implementing exception") binds `var ex: IRetryable` and calls `ex.ShouldRetry()`. Its rule sentence is unchanged; its program is rewritten to the bare arm or the guard form, and cases 13 and 14 above pin the two halves. `l9_1_0007` (`int.Parse` raises `OverflowException` on an out-of-range value) and `l6_8_0003` (a fault surfaces as `DomainFaultedException`) keep their verdicts, since each arm still matches; their titles and `rule:` headers are re-read at landing. The gtest `binding_no_shared_base_types_as_exception_spec_31_2_1_v0_243` already pins the alternative half of the binding rule.

### 9.5 Change class per part

| Part | Class | Why |
|---|---|---|
| the marker; the conformance restriction | design change | new built-in surface; one new rejection |
| the catch-binding rule | design change for the single-interface binding (a settled typing changes); a ruling for the alternative (the text did not cover an interface; the compiler's reading is stated) | |
| the kind of each existing type; the revised failure-signal rule | design change | a new normative classification of landed types |
| ▲`ParseRangeException` at `Parse`/`Convert` | design change | the class a member raises changes; every existing arm keeps matching |
| ▲`DomainFailedException` at joins | design change | the same |
| the five misfit flags | ruling | records a classification; no behaviour changes |
| the stdlib §2.5 label | ruling that touches a recorded label (§8) | |
| the undivided catch-all warning | design change | a new diagnostic; newly-warned; no promotion scheduled |
| the Appendix E row | ruling | classifies a delta the appendix did not name |
| Guide 2, the reference, the other guides | guide revalidation riding the train | |
| **ratchet** | none | |

All parts are meant to land as one revision after acceptance: the marker without the taxonomy selects nothing, and the taxonomy without the binding rule gives a boundary arm that cannot log.

### 9.6 Trackers to file at landing (`spec-tracking`)

Each is a spec-ahead-of-implementation delta: (1) the built-in `IFailure` and the conformances of the ten landed failure types; (2) the single-interface binding typed as `Exception`; (3) the conformance restriction; (4) the undivided catch-all warning; (5) `ParseRangeException` and its four raise sites; (6) `DomainFailedException` at `Domain.Spawn` joins and `Parallel`. Compiler gaps met on the way, which predate the proposal, are in §12.

### 9.7 Decline-index rows to add at graduation

`declined-at-proposal`, each pointing at this proposal's section: a base class `Failure: Exception` as the marker (§3.3; bar: evidence that the binding rule misleads readers, with a migration for the two bridges); reporting members on the marker, and an implicit `IFailure`-to-`Exception` conversion (§3.2); the kind as a virtual property, a class modifier, or a throw-site keyword (§3.2); a default of "unmarked means failure" (§4.5); re-typing a query's throwing form as a failure (§4.1; bar: a declared failure surface on members); a kind field on `ExceptionInfo` (§6.6); promoting the catch-all warning to an error (§5.4; bar: its own proposal); abandoning the process or the domain on a defect, and any rule forcing callers to handle failures (the note's candidates D and B; §31.3 and rationale §12.4 stand). And one row edited: the `DateTime` range-fault row's summary, per §8.

### 9.8 The Migration note, in outline

*Newly rejected:* a type that lists `IFailure`, or an interface extending it, and is not an `Exception`-derived class. *Newly defined:* `IFailure`; the kind of every exception type; `ParseRangeException`; `DomainFailedException`; the type of a binding over an interface arm. *Newly warned:* the undivided catch-all (no promotion scheduled). *Behaviour changes with every arm still matching:* out-of-range `Parse` input raises `ParseRangeException`; a failure leaving a domain is joined as `DomainFailedException`. *Verdict change:* a binding over a single interface arm no longer exposes the interface's members. *Doc-only:* the misfit flags; the stdlib §2.5 label. *Spec-ahead-of-impl:* the six trackers of §9.6.

## 10. Proposed text

The edits below are exact. Each `old` text occurs once in its document at v0.355, and the whole set applies in order (MEASURED: `data/edits.py check`, 46 edits, 0 anchor failures). Applied to a scratch clone, the patched documents pass `scripts/check_docs.sh` with every anchor resolving and 558 example fragments extracted, against 553 before (MEASURED). `data/edits.py` is the same set in machine-checkable form, kept so that the anchors can be re-verified at graduation; its `twin` command checks it against this section. The changelog entry, the version headers, the pins and the `DECLINED.md` rows are written at landing and are not in the set.

#### Language specification (`docs/Bezel_Language_Spec.md`)

**S1** — §2.3, the table's *Standard exception types* row: replace

````text
| Standard exception types | the [§31.4](#sec-standard-exception-types) built-in hierarchy (`Exception` and the types in its tree), whose module is `System`
````

with

````text
| Standard exception types | the [§31.4](#sec-standard-exception-types) built-in hierarchy (`Exception` and the types in its tree) and the failure marker `IFailure` ([§31.4](#sec-the-failure-marker)), whose module is `System`
````

**S2** — §31.1 *The throw operand*, end of the last paragraph: insert after

````text
Every throwable carries the `Exception` reporting surface — `Message`, `StackTrace`, `CauseChain`, `SupersededChain` ([§31.4](#sec-standard-exception-types)) — and participates in the unhandled-exception report.
````

the text

````text
 A throwable whose class implements `IFailure` is a **failure**; every other is a **defect** report, cancellation apart ([§31.4](#sec-the-failure-marker)). Dispatch reads the kind only where an arm names the marker.
````

**S3** — §31.2.1, the binding-pattern bullet: insert after

````text
- A binding pattern: `var name: Type`. Matches and binds the exception to `name` for use in the arm's guard and body. The binding is in scope only within the arm.
````

the text

````text
 `name`'s static type is `Type` where `Type` is a class or a type parameter, and `Exception` where `Type` is an interface (*Interface arm types*, below).
````

**S4** — §31.2.1, the multi-type bullet: replace

````text
When binding, `name` has the static type of the **nearest common base** of the listed types — the most specific class that is a base of every listed type. Every listed type is `Exception`-derived (the arm-type rule below), so the nearest common base is always `Exception` or a class deriving from it; for alternatives sharing nothing more specific, it is `Exception`.
````

with

````text
When binding, `name` has the static type of the **nearest common base** of the listed types — the most specific class that is a base of every listed type, an interface counting as `Exception` (*Interface arm types*, below). Every listed class type is `Exception`-derived (the arm-type rule below), so the nearest common base is always `Exception` or a class deriving from it; for alternatives sharing nothing more specific, and for every alternative that lists an interface, it is `Exception`.
````

**S5** — §31.2.1, a new paragraph after *Arm types are `Exception`-family*: insert after

````text
the remedy is to delete the arm or fix the type.
````

the text

````text
**Interface arm types.** An interface named in an arm is a filter: the arm takes exactly the throwables whose class implements it. It does not type a binding. Every throwable is an instance of an `Exception`-derived class ([§31.1](#sec-the-throw-operand)), and nothing narrower is known of one matched through an interface, so a binding arm over an interface binds `name` at `Exception`, and an interface listed in a binding alternative counts as `Exception` in the nearest-common-base computation: `var f: IFailure` and `var ex: IOException | IRetryable` each bind an `Exception`. The binding therefore reads `Message`, is an argument where an `Exception` is expected, and chains as a cause ([§31.4](#sec-standard-exception-types)), whatever interface selected it. The interface's own members are reached by a type test inside the arm — `ex as IRetryable`, or the guard form `var ex: Exception when ex is IRetryable`, whose arm sees `ex` narrowed to the interface ([§12.7](#sec-type-tests-is-is-not-as)). A type parameter used as an arm type keeps its class bound (below): `where TEx: Exception, IFailure`, never the interface alone. The failure marker `IFailure` ([§31.4](#sec-the-failure-marker)) is the interface this rule is written for.
````

**S6** — §31.2.3, a new paragraph after *An unreachable arm draws a warning*: insert after

````text
and [§19.4](#sec-guards)'s first-match order is stated there as ordering advice.
````

the text

````text
**An undivided catch-all draws a warning.** An unguarded arm whose pattern names `Exception` alone — bare or binding, not in an alternative — takes defects, failures and cancellation alike ([§31.4](#sec-the-failure-marker)). The compiler emits a **warning** at such an arm unless one of three things shows that the division was considered: an earlier unguarded arm of the same `catch` block names `IFailure`; the arm's body contains a bare re-raise ([§31.2.4](#sec-bare-re-raise)); or the arm's body is the unit value `()` ([§31.2.5](#sec-deliberate-suppression)). The warning names the two remedies: handle `IFailure` in an earlier arm, which makes this arm the defect arm, or re-raise. An arm meant to take both kinds in one body lists both — `var ex: IFailure | Exception => …` — which is not an arm naming `Exception` alone and draws nothing. The rule reads the arm patterns of one `catch` block and one arm body; it models no raise and no arm selection ([§7.4](#sec-member-access-on-nullable-values) *Narrowing across `try`*). The diagnostic is a warning and not an error: a supervisor's last arm, a test's oracle arm and a logger of last resort are well-formed catch-alls, and each has a one-line spelling that says so.
````

**S7** — §31.2.4, the last example's second arm: replace

````text
  var ex: Exception => Log(ex)
end;
````

with

````text
  var f: IFailure => Log(f)                 // every other failure; `f` is an `Exception` (§31.2.1); defects pass
end;
````

**S8** — §31.2.5, end of the last paragraph: insert after

````text
Silent suppression of exceptions is a code smell, and the syntax does not hide it.
````

the text

````text
 To suppress expected failures only, name the marker — `IFailure => ()` — and defects and cancellation still propagate ([§31.4](#sec-the-failure-marker)).
````

**S9** — §31.4, the first paragraph: replace

````text
(and `ArgumentException`/`ArgumentOutOfRangeException`'s `(message, paramName)` forms) are **`pure`**
````

with

````text
(and `ArgumentException`/`ArgumentOutOfRangeException`'s `(message, paramName)` forms, and the same two forms of `ParseRangeException`, stdlib [§9.1](Bezel_Standard_Library.md#sec-integer-types)) are **`pure`**
````

**S10** — §31.4, the hierarchy tree: replace

````text
├─ FormatException                  — string does not conform to an expected format.
````

with

````text
├─ FormatException                  — string does not conform to an expected format. A failure: implements `IFailure`.
````

**S11** — §31.4, new paragraphs after the tree's arm-ordering paragraph: insert after

````text
and an arm pattern of `ArgumentException` matches `ArgumentNullException`.
````

the text

````text
<a id="sec-the-failure-marker"></a>
**The failure marker.** An exception is of one of two kinds, and the kind is a property of its class. A **failure** reports a condition a correct program can meet: the environment, input from outside the program, a peer, or a time or capacity limit produced it. Its class implements the built-in interface `IFailure`. Every other exception is a **defect** report: the program is wrong, and the exception says where. One class is named apart: `OperationCanceledException` reports **cancellation**, the answer to a request the program itself made; it does not implement `IFailure`, and a handler of failures never takes it.

```bezel
public interface IFailure
end IFailure;
```

`IFailure` declares no members: it classifies, and the object it classifies is always reachable as an `Exception` ([§31.2.1](#sec-arm-patterns), *Interface arm types*). It is available without import, with the hierarchy above. A class implements it by listing it, and a class deriving from one that implements it implements it too ([§14.2](#sec-interface-conformance) rule 1), so a kind is inherited and cannot be shed. **Only an `Exception`-derived class may conform.** A record, an enum, a choice, or a class outside the `Exception` family that lists `IFailure`, or an interface extending it, is a compile error at the declaration: no value of such a type can be thrown ([§31.1](#sec-the-throw-operand)), so the conformance could never be observed — [§31.2.1](#sec-arm-patterns)'s ground for rejecting a dead arm type. An interface may extend `IFailure`; the restriction then binds its implementers.

Of the built-in types, `FormatException` implements `IFailure`. The standard library's failure types, and the rule by which a condition is raised as a failure or as a defect, are stated in stdlib [§1](Bezel_Standard_Library.md#sec-failure-signal-rule). A class that this specification or the standard library declares changes kind only by recorded ruling. User code chooses per class: `class ConfigError(message: string): Exception(message), IFailure` declares a failure, and a class that does not list the marker declares a defect report.

The kind is what a handler at a boundary selects on: an `IFailure` arm takes every failure and lets defects and cancellation pass ([§31.2.3](#sec-catch-ordering-and-exhaustiveness)). Nothing else in the language reads it. The ownership of a thrown exception ([§31.1](#sec-exception-ownership)), the cleanup an exception edge runs ([K3.15](#sec-kernel-k3)), a task's fault ([§30.9](#sec-exception-handling-in-async-methods)), the purity carve-out ([§22.15](#sec-pure-throw)), supersession ([§31.2](#sec-try-catch-finally)), and the reports of [§32.8](#sec-callbacks) and [§33.3](#sec-exit-code) are the same for both kinds and for cancellation.
````

**S12** — §31.4, the last paragraph: replace

````text
`FileNotFoundException`, `IOException`, and others. Outside the auto-imported modules
````

with

````text
`ParseRangeException` (deriving from `OverflowException` and implementing `IFailure`; raised by the `Parse` family for a value outside the type's range; declared in the auto-imported `System`, stdlib §9.1), `FileNotFoundException`, `IOException`, and others. Outside the auto-imported modules
````

**S13** — §30.14 *Faults*: replace

````text
whose `Message` and `FaultStackTrace` carry copies of the original exception's message and stack-trace text (`string`s — sendable). Programs that need richer fault data
````

with

````text
whose `Message` and `FaultStackTrace` carry copies of the original exception's message and stack-trace text (`string`s — sendable). The exception's kind crosses with them: where the original exception's class implements `IFailure` ([§31.4](#sec-the-failure-marker)), the join raises `DomainFailedException`, which derives from `DomainFaultedException` and implements `IFailure` (stdlib [§6.8](Bezel_Standard_Library.md#sec-domain)). An arm naming `DomainFaultedException` takes both; an arm naming `IFailure` takes a failure raised in another domain as it takes one raised in its own. Programs that need richer fault data
````

**S14** — §30.14 *Data-parallel combinators*, *Results and faults*: replace

````text
the returned task faults with `DomainFaultedException` — this section's fault vocabulary, with the no-aggregation
````

with

````text
the returned task faults with `DomainFaultedException` (`DomainFailedException` where that first fault is a failure) — this section's fault vocabulary, with the no-aggregation
````

**S15** — Appendix C.7, the multi-type binding bullet: replace

````text
- A multi-type catch alternative in binding form types the binding as the listed types' **nearest common base** — with every listed type `Exception`-derived, always `Exception` or a class deriving from it ([§31.2.1](#sec-arm-patterns)).
````

with

````text
- A multi-type catch alternative in binding form types the binding as the listed types' **nearest common base** — with every listed class type `Exception`-derived and an interface counting as `Exception`, always `Exception` or a class deriving from it; a binding arm over one interface type types the binding as `Exception` ([§31.2.1](#sec-arm-patterns)).
- `IFailure` is listed only by a class deriving from `Exception`, or extended by an interface; a record, enum, choice, or class outside the `Exception` family that conforms to it is a compile error at the declaration ([§31.4](#sec-the-failure-marker)).
````

**S16** — Appendix E, a new row after the *public type removed* row: insert after

````text
| public type removed; base class changed; implemented interface removed | **breaking** | An interface *added* to a type: widening. |
````

the text

````text
| `IFailure` added to, or removed from, the interfaces of an exception class | **breaking** | The kind is what a consumer's boundary arm selects on ([§31.4](#sec-the-failure-marker)): adding the marker makes every `IFailure` arm take the class, removing it makes them pass it. The one exception to the row above. |
````


#### Standard library (`docs/Bezel_Standard_Library.md`)

**L1** — §1, the failure-signal rule, band 1: replace

````text
(a bad argument, protocol misuse, a state violation) → a `requires` clause where a contract is possible (language spec [§22.16](Bezel_Language_Spec.md#sec-contract-clauses)); the residual defect exceptions (such as `Argument*` and `InvalidOperationException`) at pre-contract boundaries. A defect surface is never a recovery surface: its exception exists to report a bug, not to be branched on.
````

with

````text
(a bad argument, protocol misuse, a state violation, an arithmetic result outside its domain) → a `requires` clause where a contract is possible (language spec [§22.16](Bezel_Language_Spec.md#sec-contract-clauses)); the residual defect exceptions (such as `Argument*` and `InvalidOperationException`) at pre-contract boundaries, and the arithmetic classes of language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types) (`OverflowException`, `DivideByZeroException`) where a member surfaces an arithmetic fault. A defect surface is never a recovery surface: its exception exists to report a bug, not to be branched on, and its class does not implement `IFailure` (*Failures and defects*, below).
````

**L2** — §1, the failure-signal rule, band 4: replace

````text
→ a typed exception, producer-local per language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types), **regardless of how much information it carries**
````

with

````text
→ a typed exception whose class implements `IFailure` (a **failure**; *Failures and defects*, below), producer-local per language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types), **regardless of how much information it carries**
````

**L3** — §1, a new paragraph after the band-3/band-4 paragraph: insert after

````text
and the no-dual rule below applies to whatever spelling that design fixes.
````

the text

````text
**Failures and defects.** The exceptions this library raises are of the kinds language spec [§31.4](Bezel_Language_Spec.md#sec-the-failure-marker) defines, and the band fixes the kind: a band-4 condition is raised with a class that implements `IFailure`, a band-1 condition with one that does not, and cancellation with `OperationCanceledException`. The test between bands 1 and 4 is whether a correct program can meet the condition. It can meet a missing file, malformed input, a closed channel and an elapsed deadline. It cannot meet an index out of range, a key it asserted present, an empty collection it asserted non-empty, or an arithmetic result outside its domain, except by being wrong; where a caller cannot know the answer in advance, the member's band-2 query stands beside the throwing form (the tiers of the no-dual rule, below), which is why the throwing form of a query is a defect and stays one. The failure types this document declares are `IOException` and the classes deriving from it ([§7.5](#sec-io-exception-types)), `SerializationException` ([§2.15](#sec-concrete-serialization-formats)), `TimeoutException` ([§6.1](#sec-cancellationtoken-and-cancellationtokensource)), `ChannelClosedException` and `ChannelFullException` ([§6.9](#sec-channel)), `ParseRangeException` ([§9.1](#sec-integer-types)) and `DomainFailedException` ([§6.8](#sec-domain)); with the built-in `FormatException` they are the library's whole failure band. One exception class is of one kind, and a member does not raise a failure with a defect class or a defect with a failure class. No class of this library that implements `IFailure` derives from one that does not, with two recorded exceptions, each keeping an established catch arm matching: `ParseRangeException` derives from `OverflowException`, and `DomainFailedException` from `DomainFaultedException`. A landed member that raises a failure class for a band-1 condition is a recorded misfit under this rule's scoping sentence, flagged where it stands: the unrecognized format specifier ([§2.18](#sec-iformattable), [§2.6](#sec-guid), [§3.1](#sec-stringbuilder)) and the write-side serialization conditions ([§2.14](#sec-serialization-vocabulary), [§2.15](#sec-concrete-serialization-formats)). A class's kind changes only by recorded ruling, as the taxonomy grows only by one (language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)).
````

**L4** — §2.5 *Parse and range contracts*: replace

````text
— a band-4 catchable fault, not a band-1 argument defect; the constructors' component validation keeps `ArgumentOutOfRangeException`.
````

with

````text
— a band-1 defect of the arithmetic domain ([§1](#sec-failure-signal-rule)), raised with the arithmetic class and not with an argument-blaming one; the constructors' component validation keeps `ArgumentOutOfRangeException`.
````

**L5** — §2.3 `Convert`: replace

````text
and `OverflowException` when the value does not fit — the same contract as the §9 per-type `Parse` family these conversions mirror;
````

with

````text
and `ParseRangeException` ([§9.1](#sec-integer-types); an `OverflowException`) when the value does not fit — the same contract as the §9 per-type `Parse` family these conversions mirror;
````

**L6** — §9.1, *The radix alphabet*: replace

````text
and any other input raises `FormatException`, a value outside the type's range raising `OverflowException`. `ToString(radix)` emits
````

with

````text
and any other input raises `FormatException`, a value outside the type's range raising `ParseRangeException` (*The range failure*, below). `ToString(radix)` emits
````

**L7** — §9.1, a new paragraph at the end of the section: insert after

````text
and share the span forms' per-type, non-constraint status.
````

the text

````text
**The range failure.** A `Parse` member — here, in [§9.2](#sec-double-and-float) and [§9.3](#sec-decimal), and through [§2.3](#sec-convert)'s `Convert.To*` — whose input is well-formed and spells a value the type cannot represent raises `ParseRangeException`:

```bezel
public class ParseRangeException: OverflowException, IFailure
  public pure constructor();
  public pure constructor(message: string);
  public constructor(message: string, cause: Exception);
end ParseRangeException;
```

Input from outside the program is a condition a correct program can meet, so the class implements `IFailure` ([§1](#sec-failure-signal-rule)). It derives from `OverflowException` so that an `OverflowException` arm around a `Parse` call matches as it always has; the arithmetic `OverflowException` of language spec [§6.1](Bezel_Language_Spec.md#sec-integer-arithmetic) is a defect, and no arm on `IFailure` takes it. Wherever this document writes *the `FormatException`/`OverflowException` contract* of a `Parse` family, the second class raised is this one. It lives in the auto-imported `System`, beside its producers, and a user type implementing `IParsable<T>` ([§2.19](#sec-static-abstractions)) raises the same two classes.
````

**L8** — §9.2, the grammar paragraph: replace

````text
`"1_000e999"` raises `FormatException`, not the `OverflowException` below.
````

with

````text
`"1_000e999"` raises `FormatException`, not the `ParseRangeException` below.
````

**L9** — §9.2, the range paragraph: replace

````text
raises `OverflowException`: the type cannot represent the number the input spells
````

with

````text
raises `ParseRangeException` ([§9.1](#sec-integer-types)): the type cannot represent the number the input spells
````

**L10** — §9.3 `Decimal`: replace

````text
raises — raises `OverflowException`. `TryParse` returns `false` in every case where `Parse` would raise.
````

with

````text
raises — raises `ParseRangeException` ([§9.1](#sec-integer-types)). `TryParse` returns `false` in every case where `Parse` would raise.
````

**L11** — §2.15, the declaration of `SerializationException`: replace

````text
public class SerializationException: Exception

````

with

````text
public class SerializationException: Exception, IFailure
````

**L12** — §6.1, the declaration of `TimeoutException`: replace

````text
public class TimeoutException: Exception

````

with

````text
public class TimeoutException: Exception, IFailure
````

**L13** — §6.9, the two channel exception declarations: replace

````text
public class ChannelClosedException: Exception
end ChannelClosedException;

public class ChannelFullException: Exception        // sync Send on a full bounded channel
````

with

````text
public class ChannelClosedException: Exception, IFailure
end ChannelClosedException;

public class ChannelFullException: Exception, IFailure   // sync Send on a full bounded channel
````

**L14** — §7.5 IO exception types: replace

````text
All derive from `IOException`, which derives from `Exception`.
````

with

````text
All derive from `IOException`, which derives from `Exception` and implements `IFailure` (language spec [§31.4](Bezel_Language_Spec.md#sec-the-failure-marker)), so every member of the family is a failure.
````

**L15** — §6.8, the declaration block: replace

````text
  // Message carries the original exception's message text.
end DomainFaultedException;
```
````

with

````text
  // Message carries the original exception's message text.
end DomainFaultedException;

public class DomainFailedException: DomainFaultedException, IFailure   // the original was a failure
end DomainFailedException;
```
````

**L16** — §6.8, the paragraph after the declaration: replace

````text
are copied strings. `DomainFaultedException` requires `import System.Threading`,
````

with

````text
are copied strings. The exception's kind crosses with them (language spec [§30.14](Bezel_Language_Spec.md#sec-multiple-domains)): the join raises `DomainFailedException` where the original exception's class implements `IFailure`, and `DomainFaultedException` itself for a defect and for cancellation. `DomainFailedException` declares nothing of its own. It derives from `DomainFaultedException` so that an arm naming that class keeps matching every fault a join raises. Both classes require `import System.Threading`,
````

**L17** — §6.12 `Parallel`, *Results in input order*: replace

````text
first fault wins, surfaced at the `await` as `DomainFaultedException` (implementations
````

with

````text
first fault wins, surfaced at the `await` as `DomainFaultedException`, or as `DomainFailedException` where that fault is a failure ([§6.8](#sec-domain)) (implementations
````

**L18** — §2.17, end of the first paragraph: insert after

````text
one that shows constructors carries exactly those.
````

the text

````text
 Which of them are failures (language spec [§31.4](Bezel_Language_Spec.md#sec-the-failure-marker)) is stated once, in [§1](#sec-failure-signal-rule).
````

**L19** — §2.18, the specifier-vocabulary paragraph: insert after

````text
An unrecognized specifier raises `FormatException` (strict-fault, as everywhere).
````

the text

````text
 The condition is a band-1 defect raised with a failure class: a recorded misfit under [§1](#sec-failure-signal-rule)'s rule, shared by `Guid.ToString(format)` ([§2.6](#sec-guid)) and `StringBuilder.AppendFormat` ([§3.1](#sec-stringbuilder)), which stands until its own ruling.
````

**L20** — §2.14, the registry paragraph: insert after

````text
raises `SerializationException` — the closed set enforced on both directions.
````

the text

````text
 (The write-side half is a band-1 defect raised with a failure class, a recorded misfit under [§1](#sec-failure-signal-rule)'s rule, as a `Weak` reference to a target never strongly reached is.)
````


#### Design rationale (`docs/Bezel_Design_Rationale.md`)

**R1** — §11, a new section §11.12 after §11.11 (before `## 12. Async`): insert before

````text
<a id="sec-async"></a>
````

the text

````text
<a id="sec-failures-and-defects"></a>
### 11.12 Failures and defects: the kind is a marker interface on the class

(Proposal 0211.) The failure-signal rule (§11.4) separates a caller's bug from a failure consumed at a boundary. The difference is a fact about the exception's class — it implements `IFailure` or it does not (spec §31.4) — so that a handler can name it: the boundary arm names the marker, where an arm on `Exception` takes both.

**An interface, not a base class.** A base class `Failure: Exception` is the simpler thing to say and needs no rule at the catch arm. It was declined on three grounds. A class has one base, so a band drawn as a subtree cannot hold a class that must also stay under another: the out-of-range `Parse` input, which existing `OverflowException` arms take, and the fault a domain join raises, which existing `DomainFaultedException` arms take. Each would have had to leave its base, and the arms written against that base would have stopped matching. Second, moving a class between bands would be a change of base class, which spec Appendix E classifies as breaking for every consumer, where adding an interface touches only the arms that name it. Third, the precedent is against the subtree: Java drew its checked/unchecked line by base class and filed `NumberFormatException` — malformed input, the plainest failure there is — under the unchecked `IllegalArgumentException`, because its is-a parent was on the other side of the line. A kind that cuts across the is-a tree needs a mechanism that can.

**The cost of the interface, and the rule that pays it.** A binding of interface type exposes only the interface's members, so a boundary arm that bound the marker could not read `Message`, pass the exception to a logger, or chain it as a cause — the three things a boundary arm does. Declaring the reporting members on the marker fixes the first and not the other two. The rule taken is local to the catch arm and general: an interface in an arm selects, and the binding is the exception, typed at the most specific class the arm's class types determine (spec §31.2.1). It is the rule the multi-type alternative already had for classes, extended to the one case its text did not cover.

**Why the marker has no members, and only exceptions may carry it.** With the binding an `Exception`, a member on the marker would be a second way to reach what the binding already has. Conformance is restricted to `Exception`-derived classes on the ground §11.7 used for arm types: a record or a non-exception class that lists the marker is a shape no program can observe, and Bezel rejects dead shapes rather than carrying them.

**Unmarked means defect.** A user exception class is a defect report until it lists the marker. The other default would let a boundary arm swallow whatever a library author forgot to classify, which is the failure this entry exists to remove; the cost of the chosen default is one interface name on each class meant to be handled.

**What the marker does not do.** It forces nothing: no `throws` clause, no obligation to handle (spec §31.3). It changes no ownership rule, and bugs still unwind and clean up (spec K3.15) — abandoning the process or the domain on a defect was weighed and declined on §12.4's ground. It leaves the query/assert tiers alone: a missing key and an empty collection stay defects in their throwing forms, because each has a query beside it.

**Reopening bars.** For the base class: evidence that the catch-arm binding rule misleads readers, together with a migration for the two bridging classes. For re-typing a query's throwing form as a failure: a declared failure surface on members (a `fails` clause), which would let the pairs be derived rather than hand-written.
````

**R2** — §12.6, *Faults cross as `DomainFaultedException`*: replace

````text
Copying the message and stack-trace text (strings — sendable) preserves the diagnostic value;
````

with

````text
Copying the message and stack-trace text (strings — sendable) preserves the diagnostic value, and the class the join raises carries the one fact a handler selects on — failure or not ([§11.12](#sec-failures-and-defects)) — with no exception class becoming sendable;
````

**R3** — §11.4, end of the last paragraph: insert after

````text
and one mechanism per fault condition — chosen per condition by the rule, not per language by fashion.
````

the text

````text
 The line between the rule's first and fourth bands is visible in the exception's class ([§11.12](#sec-failures-and-defects)).
````


#### Guide 2 (`docs/Bezel_Guide_02_Errors_and_Nullability.md`)

**G1** — the opening list: replace

````text
- A **caller bug** is a contract (`requires`, §7) or defect exception.
- A **boundary fault** — I/O, network, anything logged or retried rather than branched on — is a typed exception (§4).
````

with

````text
- A **caller bug** is a contract (`requires`, §7) or a defect exception: a report, not something to handle.
- A **failure** — I/O, network, bad input: anything a correct program can meet, logged or retried rather than branched on — is a typed exception whose class implements `IFailure` (§4).
````

**G2** — §4, new paragraphs after the hierarchy paragraph: insert after

````text
Order arms most-specific first.
````

the text

````text
**Failures and defects.** Every exception is one of two kinds, and its class says which (spec §31.4). A *failure* is something a correct program can meet — a missing file, malformed input, a peer that went away, a deadline — and its class implements the marker interface `IFailure`. Everything else is a *defect* report: an index out of range, a broken contract, a null where none was promised. `FormatException`, the `IOException` family, `SerializationException`, `TimeoutException` and the channel exceptions are failures; `ArgumentException`, `InvalidOperationException`, `IndexOutOfRangeException`, `OverflowException`, `ContractViolationException` and their relatives are defects. (`OperationCanceledException` is neither: it answers a cancellation you asked for.)

That gives a boundary its arm. Where you would have reached for `Exception`, name the marker:

```bezel
method Serve(request: Request): Response
  try
    return Dispatch(request);
  catch
    var f: IFailure => do                    // expected: answered
      return Response.BadGateway(f.Message);
    end,
    var ex: Exception => do                  // a bug: reported, not hidden
      ReportDefect(ex);
      return Response.InternalError();
    end
  end;
end Serve;
```

The `IFailure` arm takes every failure and lets every bug through; `f` is an ordinary `Exception` inside the arm. The second arm is there only because this boundary is a supervisor that must keep serving. Most boundaries have no second arm: a bug then propagates, and an uncaught one ends the program with its report (spec §33.3). Where cancellation can reach a supervisor, give it its own arm ahead of the `Exception` arm — `OperationCanceledException => throw` — so that it is not reported as a bug. One arm on bare `Exception` that handles both draws a compiler warning (spec §31.2.3). An exception class of your own is a defect until you say otherwise:

```bezel
public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;
```

Two habits keep the kinds honest. Wrap a failure in a failure and never wrap a defect: re-raise it. And when out-of-range text matters, catch `FormatException` and `ParseRangeException` (or just `IFailure`); `ParseRangeException` is the failure `int.Parse("99999999999")` raises, distinct from the arithmetic `OverflowException` of `a * b`, which is a defect.
````

**G3** — §5, the `Handle` example: replace

````text
  catch
    var ex: Exception => RecordError(ex)
  end;
````

with

````text
  catch
    var ex: IFailure => RecordError(ex)   // ex is an Exception here; a bug propagates
  end;
````

**G4** — §7, the boundary row of the table: replace

````text
| a typed exception, producer-local (spec §31.4); read it in the `catch` and copy out what you keep (§5) |
````

with

````text
| a typed exception whose class implements `IFailure`, producer-local (spec §31.4); catch it at the boundary with an `IFailure` arm and copy out what you keep (§4, §5) |
| Handling whatever goes wrong at a boundary | an `IFailure` arm; a separate `Exception` arm only in a supervisor that reports the bug, never one arm for both (§4) |
````

**G5** — §7, the paragraph after the table: insert after

````text
Branching program logic on caught exception types at an interior call site is the signal to check the rule for a value-shaped tier.
````

the text

````text
 And at the boundary the arm names `IFailure`: an arm on bare `Exception` takes the bugs too.
````


#### Code-generation reference (`docs/Bezel_For_Code_Generation.md`)

**C1** — §6, *Declaring, throwing and catching an exception type*: replace

````text
Derive from `Exception` (or any built-in exception type) and chain the message to the base; a caught exception is a **borrow** valid inside its arm only, so copy out what you keep.

```bezel
public class ConfigError(message: string): Exception(message)
end ConfigError;
````

with

````text
Derive from `Exception` (or any built-in exception type) and chain the message to the base; list `IFailure` when the condition is one a correct program can meet, since an unmarked class is a defect report (§31.4); a caught exception is a **borrow** valid inside its arm only, so copy out what you keep.

```bezel
public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;
````

**C2** — §7.9, a new pair at the end of the section (before `## 8.`): insert after

````text
var port := ParsePort(raw).UnwrapOr(8080);          // a constant fallback is fine eagerly
```
````

the text

````text
```bezel
try Serve(request); catch
  var ex: Exception => Respond(500);         // WARNING: one arm for bugs and failures alike
end;
try Serve(request); catch
  var f: IFailure => Respond(502, f.Message),                  // write: failures are answered
  var ex: Exception => do ReportDefect(ex); Respond(500); end  //   and bugs are reported
end;
```

> §31.4, §31.2.3: an exception whose class implements `IFailure` is a failure (`FormatException`, the `IOException` family, `SerializationException`, `TimeoutException`, `ChannelClosedException`, `ChannelFullException`, `ParseRangeException`, `DomainFailedException`); every other is a defect report. The boundary arm is `IFailure`; its binding is an `Exception`. Out-of-range text at `Parse` raises `ParseRangeException` (an `OverflowException`), not a bare `OverflowException`; a failure in a spawned domain arrives as `DomainFailedException` (a `DomainFaultedException`).
````

## 11. The condition table

Every condition for which a standard-library section says a member raises a named exception type, by section (115 rows; generated from `data/taxonomy.tsv` by `data/taxonomy_check.py --markdown`). *Kind* is the kind of the condition under §4.1. *Action*: **none** — the type's kind already matches; **marker** — the type implements `IFailure`, directly or by inheritance; **range-class** — the member raises ▲`ParseRangeException`; **join-class** — the join raises ▲`DomainFailedException` when the original was a failure; **misfit** — a defect condition raised with a failure class, flagged and not moved; **relabel** — the section's band word changes, the type does not.

| Stdlib § | Condition | Type today | Kind | Action |
|---|---|---|---|---|
| 1 | a negative `capacity`, `size` or `length` constructor argument (#1396) | `ArgumentOutOfRangeException` | defect | none |
| 1 | `Channel<T>(capacity)` with `capacity < 1` (restated at 6.9) | `ArgumentException` | defect | none |
| 2.1 | `Console.ReadKey()` at end of input | `EndOfStreamException` | failure | marker |
| 2.2 | `Math.Clamp` with `low > high` | `ArgumentException` | defect | none |
| 2.2 | `Math.DivRem(a, 0)` | `DivideByZeroException` | defect | none |
| 2.3 | `Convert.ToString(value, toBase)` with a base outside 2, 8, 10, 16 | `ArgumentException` | defect | none |
| 2.3 | `Convert.To*`: the text is not a well-formed value | `FormatException` | failure | marker |
| 2.3 | `Convert.To*`: the value does not fit the target type | `OverflowException` | failure | range-class |
| 2.4 | `Environment.Set` with an empty name or one containing `=` | `ArgumentException` | defect | none |
| 2.5 | a `DateTime`/`DateOnly`/`TimeOnly` constructor component outside its range | `ArgumentOutOfRangeException` | defect | none |
| 2.5 | `TimeSpan` `*`, `/`, unary `-` and the `From*` factories: a result outside the tick range, or a `NaN` operand | `OverflowException` | defect | none |
| 2.5 | `TimeSpan / 0.0` | `DivideByZeroException` | defect | none |
| 2.5 | `DateTime.Parse`, `DateOnly.Parse`, `TimeOnly.Parse`: any other input | `FormatException` | failure | marker |
| 2.5 | `DateTime.Add*`, `Add`, `Subtract(span)`, `FromTicks`: a result outside `MinValue..MaxValue` | `OverflowException` | defect | relabel |
| 2.6 | `Guid.FromByteArray` with `Count <> 16` | `ArgumentException` | defect | none |
| 2.6 | `Guid.Parse`: any other input | `FormatException` | failure | marker |
| 2.6 | `Guid.ToString(format)`: an unrecognized specifier | `FormatException` | defect | misfit |
| 2.7 | `Random.Next` with `maxValue < 0` or `minValue > maxValue` | `ArgumentOutOfRangeException` | defect | none |
| 2.13 | `Type.EnumNames` / `EnumValues` on a non-enum `Type` | `InvalidOperationException` | defect | none |
| 2.14 | synthesized `Deserialize`: an unregistered discriminator, or malformed input | `SerializationException` | failure | marker |
| 2.14 | synthesized `Serialize`: an unregistered run-time type, or a `Weak` reference to a target never strongly reached | `SerializationException` | defect | misfit |
| 2.15 | read side: malformed input, a type mismatch, an out-of-range number, a depth-limit violation, an unknown field under the strict policy, trailing data | `SerializationException` | failure | marker |
| 2.15 | JSON write side: a non-finite float (the section already flags it) | `SerializationException` | defect | misfit |
| 2.18 | `ToString(format)` and interpolation specs: an unrecognized specifier | `FormatException` | defect | misfit |
| 2.19 | `IParsable<T>.Parse`: bad input | `FormatException` | failure | marker |
| 2.19 | `ISteppable<T>.Step`: a result not representable in `T` | `OverflowException` | defect | none |
| 3.1 | `StringBuilder.AppendFormat`: a malformed format string, a hole index outside `args`, a spec on a type that accepts none | `FormatException` | defect | misfit |
| 3.1 | `StringBuilder.Insert` / `Remove`: an index or window outside `0..Length` | `IndexOutOfRangeException` | defect | none |
| 3.1 | `StringBuilder.Replace` with an empty `oldValue` | `ArgumentException` | defect | none |
| 3.2 | `Encoding.GetString`: ill-formed bytes; `GetBytes`: a code point the encoding cannot represent | `FormatException` | failure | marker |
| 3.2 | `GetBytes` into a destination span that is too short | `IndexOutOfRangeException` | defect | none |
| 3.3 | `ReadOnlyStringSpan` indexer and slices out of range | `IndexOutOfRangeException` | defect | none |
| 3.3 | `ReadOnlyStringSpan.CopyTo` with a length mismatch | `ArgumentException` | defect | none |
| 4.1 | a mutating member of a user-defined read-only collection | `InvalidOperationException` | defect | none |
| 4.3 | `Pop`, `PopFirst`, `PopLast`, `Dequeue`, `Peek` on an empty collection (query duals: the `Try*` forms) | `InvalidOperationException` | defect | none |
| 4.3 | `PopAt(index)` outside `0..<Count` | `IndexOutOfRangeException` | defect | none |
| 4.4 | `Array<T>` constructors with a negative capacity or size | `ArgumentOutOfRangeException` | defect | none |
| 4.4 | the indexer and the slice windows out of range | `IndexOutOfRangeException` | defect | none |
| 4.4 | a structural mutation from inside the collection's own sort comparer | `InvalidOperationException` | defect | none |
| 4.5 | `OwningArray<T>(capacity)` with a negative capacity | `ArgumentOutOfRangeException` | defect | none |
| 4.5 | the indexer out of range | `IndexOutOfRangeException` | defect | none |
| 4.8 | `dict[key]` on an absent key, compound assignment included (query duals: `GetOrNull`, `TryGetValue`) | `KeyNotFoundException` | defect | none |
| 4.8 | `Add(key, value)` / `Add(entry)` on a present key (query dual: `TryAdd`) | `ArgumentException` | defect | none |
| 4.8 | a structural mutation from inside `GetOrAdd`'s factory; the note on `KeyNotFoundException`'s base | `InvalidOperationException` | defect | none |
| 4.9 | `OwningDictionary.Add` on a present key | `ArgumentException` | defect | none |
| 4.9 | the indexer on an absent key | `KeyNotFoundException` | defect | none |
| 4.13 | `TakeCurrent()` out of protocol | `InvalidOperationException` | defect | none |
| 4.14 | indexers, slices, the `Read*`/`Write*` accessors and bit views out of range (query duals: `TryRead*`, `TryWrite*`) | `IndexOutOfRangeException` | defect | none |
| 4.14 | `MoveFrom` / `CopyFrom` with a length mismatch | `ArgumentException` | defect | none |
| 4.14 | a bit `count` outside 1..64 | `ArgumentOutOfRangeException` | defect | none |
| 4.14 | `AlignedTail()` on a view that is not byte-aligned | `InvalidOperationException` | defect | none |
| 4.14 | `span[i]!` on a null slot | `NullReferenceException` | defect | none |
| 4.14 | a read through a span whose slot value was disposed (checked disposal mode) | `DisposedAccessException` | defect | none |
| 4.15 | indexers and slices out of range | `IndexOutOfRangeException` | defect | none |
| 4.15 | `CopyFrom` with a length mismatch | `ArgumentException` | defect | none |
| 4.15 | a bit `count` outside 1..64 | `ArgumentOutOfRangeException` | defect | none |
| 4.15 | `AlignedTail()` on a view that is not byte-aligned | `InvalidOperationException` | defect | none |
| 4.17 | the indexer on an absent key | `KeyNotFoundException` | defect | none |
| 4.17 | `Add` on a present key | `ArgumentException` | defect | none |
| 4.17 | `FirstKey` / `LastKey` on an empty dictionary; a structural modification during iteration | `InvalidOperationException` | defect | none |
| 4.18 | `First` / `Last` on an empty set | `InvalidOperationException` | defect | none |
| 4.19 | the `FrozenArray<T>` indexer out of range | `IndexOutOfRangeException` | defect | none |
| 4.21 | `OwningSlotMap<T>(capacity)` with a negative capacity | `ArgumentOutOfRangeException` | defect | none |
| 4.21 | `Get` / `PopValue` on an absent handle (query duals: `GetOrNull`, `TryGetValue`, `TryPopValue`) | `KeyNotFoundException` | defect | none |
| 4.21 | a handle minted by another map | `ArgumentException` | defect | none |
| 4.21 | a mutation through a live view | `InvalidOperationException` | defect | none |
| 5 | a second enumeration of a single-consumption query; `First`/`Last`/`Single` (and predicate forms), `Min`/`Max`/`Average` with no element (query duals: the `*OrNull` forms) | `InvalidOperationException` | defect | none |
| 5 | `ElementAt` out of range (query dual: `ElementAtOrNull`) | `IndexOutOfRangeException` | defect | none |
| 5 | `ToDictionary` on a duplicate key | `ArgumentException` | defect | none |
| 5 | `Sum` past the element type's range | `OverflowException` | defect | none |
| 5.1 | `Min()` / `Max()` / `Average()` on an empty source | `InvalidOperationException` | defect | none |
| 6.1 | `Register` on a published source; publishing a source that holds registrations | `InvalidOperationException` | defect | none |
| 6.1 | `ThrowIfCancellationRequested()` | `OperationCanceledException` | cancellation | none |
| 6.2 | a blocking acquire where a domain continuation would park | `InvalidOperationException` | defect | none |
| 6.2 | a cancelled `AcquireAsync` / `TryAcquireAsync` | `OperationCanceledException` | cancellation | none |
| 6.3 | a blocking acquire where a domain continuation would park | `InvalidOperationException` | defect | none |
| 6.3 | a cancelled async acquisition | `OperationCanceledException` | cancellation | none |
| 6.3 | `new Semaphore(initial, max)` with `max < 1` or `initial > max` | `ArgumentException` | defect | none |
| 6.4 | an `AtomicInteger<T>` operation whose result is not representable | `OverflowException` | defect | none |
| 6.5 | a cancelled handler task under `RaiseAsync` | `OperationCanceledException` | cancellation | none |
| 6.7 | `WhenAny` over an empty array | `ArgumentException` | defect | none |
| 6.7 | a second `GetAsyncEnumerator` on a `WhenEach` sequence; the `FromException` example | `InvalidOperationException` | defect | none |
| 6.7 | `FromCancelled`, and a cancelled input at `WhenAll` / `WhenEach` | `OperationCanceledException` | cancellation | none |
| 6.8 | the join of an entry that raised | `DomainFaultedException` | carrier | join-class |
| 6.9 | `Channel<T>(capacity)` with `capacity < 1` | `ArgumentException` | defect | none |
| 6.9 | the synchronous `Send` on a full bounded channel | `ChannelFullException` | failure | marker |
| 6.9 | `Receive` after the drain of a closed channel; `Send`/`SendAsync` after close; a `SendAsync` pending at close | `ChannelClosedException` | failure | marker |
| 6.9 | a second concurrent consumption of the single-consumer end | `InvalidOperationException` | defect | none |
| 6.10 | `TaskScope`: `DisposeAsync` without `Join`, `Spawn` after close | `InvalidOperationException` | defect | none |
| 6.11 | a blocking form where a domain continuation would park | `InvalidOperationException` | defect | none |
| 6.11 | a cancelled pending acquisition | `OperationCanceledException` | cancellation | none |
| 6.12 | `Parallel.*` with `degree < 0` | `ArgumentException` | defect | none |
| 6.12 | the first body fault, at the `await` | `DomainFaultedException` | carrier | join-class |
| 7.1 | a `Read`/`Write` window outside the buffer's `Count` | `IndexOutOfRangeException` | defect | none |
| 7.2 | `FileStream` construction: the file does not exist under `FileMode.Open` | `FileNotFoundException` | failure | marker |
| 7.2 | `FileStream` construction: a directory component does not exist | `DirectoryNotFoundException` | failure | marker |
| 7.2 | `FileStream` construction: the OS reports a too-long path | `PathTooLongException` | failure | marker |
| 7.2 | `FileStream` construction: every other OS-level open failure | `IOException` | failure | marker |
| 7.3 | the `File` text members: ill-formed UTF-8 on read | `FormatException` | failure | marker |
| 7.4 | `Directory.Delete` of a non-empty directory without `recursive`; the other `Directory`/`Path` failures | `IOException` | failure | marker |
| 7.4 | the other `Directory`/`Path` failures | `FileNotFoundException` | failure | marker |
| 9.1 | integer `Parse` (string, radix and span forms): input outside the grammar | `FormatException` | failure | marker |
| 9.1 | integer `Parse`: a value outside the type's range | `OverflowException` | failure | range-class |
| 9.1 | `Parse` / `TryParse` / `ToString` with a radix outside 2, 8, 10, 16 | `ArgumentException` | defect | none |
| 9.2 | `double.Parse` / `float.Parse`: input outside the grammar | `FormatException` | failure | marker |
| 9.2 | `double.Parse` / `float.Parse`: a value that is not finite once rounded | `OverflowException` | failure | range-class |
| 9.3 | `decimal.Parse`: input outside the grammar | `FormatException` | failure | marker |
| 9.3 | `decimal.Parse`: a value outside `decimal`'s range | `OverflowException` | failure | range-class |
| 9.3 | `Round(digits)` with `digits` outside 0..28 | `ArgumentOutOfRangeException` | defect | none |
| 9.5 | `char.Parse`: any input but one code point | `FormatException` | failure | marker |
| 9.6 | `Repeat` with `count < 0`; `PadLeft`/`PadRight` with `totalWidth < 0` | `ArgumentOutOfRangeException` | defect | none |
| 9.6 | `IndexOf` start, and the window members, out of range | `IndexOutOfRangeException` | defect | none |
| 9.8 | `GetEnumerator` on an open-ended range | `InvalidOperationException` | defect | none |
| 10.3 | a copy window outside the destination's `Count` | `IndexOutOfRangeException` | defect | none |
| 10.4 | `string.FromCStringPointer(IntPtr.Zero)` / `FromWStringPointer(IntPtr.Zero)` | `ArgumentNullException` | defect | none |

The language's own raise sites are §31.4's tree and need no table: every built-in operation that raises (an indexer, arithmetic, a checked conversion, `!`, a non-exhaustive `case` expression, a contract clause, static initialization) raises a defect; `await` on a cancelled task raises cancellation.

## 12. Probes and data

All under [`docs/research/error-model/probes-0211/`](../research/error-model/probes-0211/). Probes are run with r40 by `run_all.sh`, which writes `RESULTS.txt`. r40 has no built-in marker, so each probe declares `interface IFailure end IFailure;` itself: what is measured is the library-convention form of the design, as in the note's p16.

| Probe | Shows | Result on r40 |
|---|---|---|
| f01 | the three marker shapes and what a boundary arm can do with each binding | runs: the interface binding reads `ToString()` only; a base-class binding reads `Message`, is passed and chains |
| f02 | an interface property satisfied by a property inherited from a user base class (§14.2 rule 1) | runs |
| f03 | the same with `Exception` as the base | **rejected**: "a field does not satisfy a property contract" (a compiler gap against §14.2 rule 1) |
| f04 | a marker binding cannot read `Message` or chain as a cause, under today's rule | both lines rejected |
| f05 | six arm forms over the marker: before the catch-all, in an alternative, after a class arm, before a class arm, guarded, bare re-raise | runs; no unreachable-arm warning for the fourth form (a compiler gap against §31.2.3) |
| f06 | a binding alternative that lists an interface | runs; the binding reads `Message`, so it is typed `Exception` |
| f07, f08 | a generic arm parameter bounded `Exception, IFailure`; bounded by the marker alone | runs; rejected as §31.2.1 specifies |
| f09 | a failure and a bug raised in a spawned domain | both arrive as `DomainFaultedException`; the marker arm takes neither |
| f10 | the proposed join class, declared by hand: `DomainFailedException: DomainFaultedException, IFailure` | the marker arm and an existing `DomainFaultedException` arm both take it |
| f11 | the kind through `await`, two awaiters, `Task.WhenAll` | runs; `WhenAll` re-raises the first input's failure although the second input's bug completed first |
| f12 | cleanup on a failure edge and a defect edge; an owned payload under a bare marker arm; `throw x` | identical order; payload disposed after the arm |
| f13 | what a marker arm takes and what passes it | takes the two marked classes; passes a contract violation, an index fault, a null fault and a cancellation |
| f14 | a `finally` raising a failure while a bug propagates | the marker arm takes the cleanup failure |
| f15 | a record and a plain class listing the marker | both compile today (the shape §3.3 rejects) |
| f16 | `throw f` with a marker-typed operand | rejected (§31.1) |
| f17 | a `sealed` marked exception class as a spawn argument | rejected: not sendable (§30.14) |
| f18 | an interface-typed value passed to a class-typed parameter | **accepted**, against §5.1; with a non-exception object the callee reads an empty `Message` (a compiler gap; the proposal does not rely on it) |
| f19 | the guide's idioms: a failure class with a primary constructor; a supervisor boundary; `IFailure \| Exception`; `IFailure => ()` | runs |
| f20 | the straddling types in r40's own library | `Parse` out of range and an arithmetic overflow reach one `OverflowException` arm; malformed input and a bad format specifier reach one `FormatException` arm |

Data (`data/`):

| File | Content |
|---|---|
| `catch_arms.py` → `catch_arms_summary.json`, `undivided_lit_files.txt` | every catch arm in the four corpora, found by walking each `catch` block, with its body classified; the undivided catch-alls of §5.4; the 40 everyday programs |
| `band_reach.py` → `band_reach.json`, `overflow_over_parse_files.txt` | the reach numbers of §2, §3.2 and §9.1 |
| `taxonomy.tsv`, `taxonomy_check.py` | the condition table and its completeness check against the stdlib document |
| `edits.py` | the edit set of §10: `check`, `apply`, `render`, `twin` |

To regenerate (the document-example corpus needs an exported copy, as in the note):

```text
X=/var/tmp/bezel-scratch/ff
mkdir -p $X/export/tests/conformance
git archive HEAD docs scripts/extract_doc_examples.py | tar -x -C $X/export
python3 scripts/extract_doc_examples.py --repo-root $X/export
D=docs/research/error-model/probes-0211/data
python3 $D/catch_arms.py . $X/export $X/data
python3 $D/band_reach.py . $X/export $X/data
python3 $D/taxonomy_check.py docs/Bezel_Standard_Library.md $D/taxonomy.tsv
python3 $D/edits.py check .
python3 $D/edits.py twin docs/proposals/0211-failure-marker-and-boundary-arm.md
docs/research/error-model/probes-0211/run_all.sh
```

**Compiler gaps met, informational, not filed.** (1) f18: an interface-typed argument is accepted at a class-typed parameter. (2) f03: a property inherited from `Exception` does not satisfy an interface property. (3) f05: no unreachable-arm warning where an interface arm covers a later class arm. (4) `ToString()` on the binding of a multi-type alternative is rejected (the diagnostic cites #2457). (5) A native callback segfaults, including rule case `s32_8_0021`'s program. The note's list (`SupersededChain` absent, `Exception.ToString()` without the message, `FaultStackTrace` unimplemented) still holds on r40.

## 13. Limits and self-critique

- **No adversarial critique has been run.** [`spec-change-process.md`](spec-change-process.md) §4 requires one before a landing; the status says so.
- **The test of §4.1 has judgement calls.** `ChannelFullException` could be read as a caller's choice of the wrong `Send`; `ToDictionary` on a duplicate key and `Guid.FromByteArray` on a wrong length depend on data; `TypeInitializationException` hides a failure behind a defect. Each is ruled in §4 with its reason, and Q5 lists them for the owner.
- **The warning's evidence is thin.** 566 arms are test oracles and the documents' own examples; the 40 everyday programs have none. Q8.
- **The table covers what the standard library names.** A condition a section leaves unstated is not in it. Exception types declared by proposals that have not landed are not classified here: a grep of proposals 0001–0139 finds 24 such classes, 14 of which derive from the `IOException` family or from `FormatException` and would be failures by inheritance; the other ten (`JsonException` and its three subclasses, `HttpResponseException`, `CryptographicException`, the two time-zone classes, `AssertionException`, `VerificationException`) state their kind when their proposal graduates (MEASURED, by grep).
- **A bug can sit behind a failure in two places, both as today.** `Task.WhenAll` re-raises the first non-successful input in input order (§6.2), and a cleanup failure supersedes a defect (§6.6). The marker makes both more visible; neither is changed.
- **The proposed binding form does not compile on r40**, which binds a single interface arm at the interface (f04). The idioms were run with the forms r40 accepts (f19); the alternative form shows the proposed typing (f06).
- **FFI could not be probed** (§6.5). §32.8's text is cited instead.
- **Prior art is from memory.** The Java observation in §3.3 and in the proposed rationale text should be checked against a source before landing.
- **Conformance level.** §31 is Core and §30.14 is in the Asynchronous-programming feature (Appendix D), so the marker and the arm rule are Core and ▲`DomainFailedException` is Extended. No Appendix D text changes.
- **Proposal 0110** (under review) touches `DomainFaultedException`; §6.3 states how the two compose, and whichever lands second re-reads the other.

## 14. Owner questions

**Q1 — The word for the bug band.** Normative text says *defect* and *failure*, and *fault* keeps meaning "an exception was raised" (330 uses in the spec and stdlib). The alternative is to adopt *fault* for the bug band and sweep those uses. **Recommendation: *defect*.**

**Q2 — The marker's shape.** An interface ▲`IFailure` with no members, or a base class `Failure: Exception`. The interface keeps every existing arm matching and makes a later re-banding an added interface; the base class needs no catch-arm rule and flips 26 `OverflowException` arms and the failure-carrying `DomainFaultedException` arms. **Recommendation: the interface**, and the name `IFailure`.

**Q3 — The catch-binding rule.** A binding arm over an interface binds an `Exception` (as r40 already does for an alternative), at the cost of one rule-corpus case and of reaching an interface's own members through the guard form. The alternative keeps §31.2.1 as it is and declares `Message`, `StackTrace` and the chains on the marker; the binding then still cannot be passed or chained. **Recommendation: the binding rule.**

**Q4 — Who may implement the marker.** Only `Exception`-derived classes, as a compile error at the declaration; or no restriction, since a non-exception can never reach an arm. **Recommendation: the restriction** (the reject-dead-shapes posture of §31.2.1).

**Q5 — The taxonomy.** Confirm the test ("a correct program can meet it") and these rulings: (a) the throwing form of a query — missing key, empty collection, index — stays a defect; (b) arithmetic outside its domain is a defect, including the `DateTime` range fault, whose stdlib §2.5 label moves from band 4 to band 1 with its type unchanged; (c) `ChannelClosedException`, `ChannelFullException` and `TimeoutException` are failures; (d) `TypeInitializationException` is a defect whatever its inner exception; (e) `OperationCanceledException` is neither. **Recommendation: as tabled in §4 and §11.**

**Q6 — The two bridging classes.** ▲`ParseRangeException: OverflowException, IFailure` and ▲`DomainFailedException: DomainFaultedException, IFailure`, each a failure class under a defect class so that existing arms keep matching. The alternative is a clean cut: out-of-range input under `FormatException` (26 arms in 13 files flip) and a sibling join class (the arms on `DomainFaultedException` stop taking failures). **Recommendation: the bridges, with these names.**

**Q7 — The five misfits.** A bad format specifier (stdlib §2.18, §2.6, §3.1) and the write-side serialization conditions (stdlib §2.14, §2.15) are defects raised with a failure class. Flag them now and move each by its own ruling, or re-type the specifier condition to `ArgumentException` in the same landing (at least 36 arms in 9 files flip). **Recommendation: flag now.**

**Q8 — The undivided catch-all.** Adopt the warning of §5.4 with the marker, warn-only, with no promotion scheduled and `IFailure | Exception` as the spelling for "both, on purpose"; or teach the idiom in the guides and add no compiler rule until programs outside the test corpora show the habit. **Recommendation: adopt the warning.** If the owner prefers the second, S6 and conformance cases 16–21 drop out and nothing else changes.

**Q9 — User classes.** A user exception class is a defect until it lists the marker. **Recommendation: yes.**

**Q10 — Three things left out on purpose.** (a) Re-raising a defect ahead of a failure at `Task.WhenAll` and `TaskScope`, which would change §30.10's first-in-input-order contract; (b) a kind field on `ExceptionInfo`; (c) any rule for a cleanup failure that supersedes a defect, beyond the guard idiom of §6.6. **Recommendation: leave all three out; say if any is wanted as its own proposal.**

**Q11 — One landing.** The marker, the taxonomy, the two classes, the binding rule and the warning in one revision, with the six trackers of §9.6; or the marker and taxonomy first and the domain join second, as the note's stage 1b had it. **Recommendation: one revision**, after an adversarial critique of this text.
