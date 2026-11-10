# Proposal 0211 — Failures and defects: a marker for the failure band, and the boundary catch arm

| | |
|---|---|
| **Status** | under-review — draft, revision 2 (2026-11-10): revised after an adversarial critique rejected revision 1 as written and judged the design sound; revision 2 has not been re-critiqued; it amends no normative document |
| **Tracking issue** | to be filed by the coordinator |
| **Target documents** | **spec** — §2.3 (one table row), §31.1 (one sentence), §31.2.1 (the catch-binding rule), §31.2.4 (one example arm), §31.2.5 (one sentence), §31.4 (the marker; one tree line), §30.14 (*Faults*; the combinators' fault sentence), Appendix C.7 (two bullets), Appendix E (one row). **stdlib** — §1 (the failure-signal rule: bands 1 and 4, one new paragraph; the two glosses of the throwing tier), §2.1, §2.3, §2.5, §2.14, §2.15, §2.17, §2.18, §3.2, §6.1, §6.8, §6.9, §6.12, §7.5, §9.1–§9.3. **rationale** — a new §11.12; one sentence each in §11.4 and §12.6. **Guide 2**, **Guide 4** (two sentences), **Guide 6** (four edits), the **Tour** (one sentence). **code-generation reference** — §5.1, §5.2, §5.6, §5.9, §6. **proposals** — `README.md` (this row), `DECLINED.md` (the rows of §9.7, at graduation). No grammar change, no new keyword, no new diagnostic other than two rejections at a declaration. |
| **Written against** | spec, stdlib and rationale **v0.355** (tree `a77e4718a`); probes on compiler release **r40** (`bezelc 0.0.0-a30bb11d`) |
| **Origin** | The research note [`docs/research/error-model/README.md`](../research/error-model/README.md) ("the note"), stages 0–1 of its §8.5. Owner direction, 2026-11-10: "Proceed with Fable research and proposals for fault/failure markers." |
| **Change class** | Design change ([`spec-change-process.md`](spec-change-process.md) §2). §9.5 gives the class of each part. Nothing here is a ratchet. |

*Conventions. An unprefixed § is a language-spec section at v0.355; stdlib, rationale, guide and note sections are marked. Quotations are verbatim, with link markup dropped. ▲ marks a name this proposal introduces. Evidence labels: **MEASURED** — produced by a script or probe under [`docs/research/error-model/probes-0211/`](../research/error-model/probes-0211/) and re-runnable (§13); **QUOTED** — text of a repository document, cited; **HYPOTHESIS** — a judgement or prediction. Probe results describe r40, which implements an older revision than the spec by design; they are informational.*

*Revision 2, against revision 1 (`fdb7e52a0`). (1) The kind of a condition is fixed by where the condition comes from, not by the tier that raises it; the three glosses that call a throwing form "a bug at this call site" are rewritten, and `dict[key]` and `First()` are defects because the program owns the state tested, not because a query exists (§4.1). (2) §10 is complete against a scripted audit of every affected sentence in the documents: 71 sites, each edited or left with a stated reason (§9.2). (3) A class under `OperationCanceledException`, or under any defect class of the documents, may not list the marker (§3.3). (4) The remedy for reaching an interface's own members is `as` inside the arm; the guard form loses the `Exception` surface (§5.2). (5) The catch-binding rule is stated as a difference from `case` (§5.2), and the v0.292 band label of the `DateTime` range fault is stated as reversed, not reworded (§8). (6) The catch-all warning is no longer proposed: it is designed, corrected and deferred to its own proposal (§5.4, §12). (7) The cost of leaving a misfit is stated (§4.4). Four owner questions are added (§15).*

## 0. Summary

Bezel raises exceptions for two different things: a bug in the program, and a problem a correct program can meet (a missing file, malformed input, a peer that went away). The standard library's failure-signal rule already separates the two when it classifies a condition, but nothing a `catch` arm can name carries the difference, so the arm written at a boundary is `Exception`, which takes both. This proposal makes the difference a fact about the exception's class.

- **The marker.** A built-in interface ▲`IFailure`, with no members. A class that implements it is a *failure*; every other exception is a *defect* report; `OperationCanceledException` stays apart as cancellation. Only an `Exception`-derived class may implement it, and not one under a class the documents declare as cancellation or as a defect (§3).
- **The boundary arm.** `IFailure => …` is an ordinary interface arm, which §31.2.1 already admits. One rule changes: a binding arm over an interface binds the exception as an `Exception`, so `var f: IFailure => Log(f.Message)` works (§5.2).
- **The test.** A condition is a failure when it comes from outside the program — the environment, data being read in, a peer, a limit — and a defect when it is a fact about what the program passed, the state it holds, or its arithmetic. The tier that raises it does not decide (§4.1).
- **The taxonomy.** Every exception type and every condition the standard library names is classified: 12 failure types, 17 defect types, cancellation, one fatal type; 116 conditions in 54 sections (§4, §11). Two classes are added so that conditions raised today with a defect class can be failures without breaking an arm: ▲`ParseRangeException: OverflowException, IFailure` for out-of-range `Parse` input, and ▲`DomainFailedException: DomainFaultedException, IFailure` for a failure that leaves a spawned domain.
- **The catch-all.** An `Exception` arm keeps its meaning. The guides and the reference teach `IFailure` at a boundary. A compiler warning for an undivided catch-all is designed in §12 and not proposed here (§5.4).
- **Nothing else moves.** Ownership on the exception edge, task faults, supersession, purity and the FFI abort are the same for both kinds (§6). No declined alternative is re-proposed; two recorded positions change and one is diverged from, and §8 names them. The call-site `(try E)` form is not proposed and nothing here depends on it (§7).

Measured reach on the four program corpora: no catch arm changes which exceptions it takes; one rule-corpus case changes its program; nothing is newly warned (§9.1).

## 1. Scope and words

### 1.1 Scope

In scope: the marker, the catch-arm rule it needs, the classification of every stdlib exception type and condition, the domain join, and the guide and reference text. Out of scope: the `(try E)` atom of the note's §8.1 item 3 (it stays behind the note's §8.5 gate), retiring any `Try…` or `…OrNull` member, a `fails` clause, any change to what a `Result` is for, and a compiler diagnostic on catch-all arms.

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
| 4. User classes | `class E: Exception, IFailure` | the same | `class E: Failure`; a class under another base cannot join |
| 5. Generic bound | `where TEx: Exception, IFailure` (f07); the interface alone is rejected at the arm, as specified (f08) | the same | `where TEx: Failure` |
| 6. New rules elsewhere | none | none | none |
| Moving a type between bands later | add or remove an interface: only arms naming the marker change | the same | change the base class: every arm naming an ancestor changes; "base class changed" is **breaking** in Appendix E, where "An interface *added* to a type" is widening (QUOTED) |
| Existing arms that stop matching, if the two bridged conditions join the band | 0 | 0 | 26 `OverflowException` arms over a `Parse` call, in 13 files, plus the arms on `DomainFaultedException` whose child raises a failure (MEASURED, `band_reach.json`) |

Three more shapes were considered and are not in the table because they fail requirement 1 as a *type*: a virtual `IsFailure` property on `Exception` (an arm can test it only in a guard, so the unreachable-arm rule and generic arms cannot see it); a `failure` modifier on the class (a new keyword for what an interface already says); and a band chosen at the `throw` site (one class in two bands, which is the straddler problem made permanent).

### 3.3 Decision

**An interface, with no members, plus one rule at the catch arm and two restrictions on who may list it.**

- ▲`IFailure` is a built-in interface declaring nothing (text: S10 in §10).
- **The binding rule** (S3–S5): an interface in an arm selects which exceptions the arm takes; it does not type the binding. A binding arm over an interface binds an `Exception`. This closes the gap in column (A), row 2, and it is not a new idea in the implementation: for a *multi-type* alternative that lists an interface, r40 already types the binding as `Exception` (f06), and a gtest pins it (`tests/sema/catch_multitype_no_common_base_v182_test.cpp`: "under v0.243 the binding types as `Exception`, which is what makes `e.Message` resolve", QUOTED). The spec's sentence for that case says "Every listed type is `Exception`-derived", which an interface is not; the rule states what the compiler does there and extends it to the single-interface binding. §5.2 gives its costs.
- **Only an `Exception`-derived class may implement the marker** (S10). A record or a plain class that lists it can never be thrown, so the conformance is unobservable (f15 shows both compile today). The precedents are Appendix C.3's rule that records "may not implement `IDisposable`" (QUOTED; K1.7) and §31.2.1's rejection of a provably dead arm type.
- **A class may not contradict the kind of a class the documents declare** (S10, S14). A class that does not inherit the marker may list it only where no class in its base chain, other than `Exception` itself, is a built-in or standard-library exception class. Built-in exception classes are open to user subclasses (stdlib §1, *Extensibility*), so without this rule `class C: OperationCanceledException, IFailure` is legal and a failure arm takes a cancellation, and `class S: InvalidOperationException, IFailure` is legal and every state-defect arm takes a user's failure (MEASURED: both compile and behave so on r40, f23). The second is the swallowing that stdlib §4.8 keeps `KeyNotFoundException` clear of. The library's two bridging classes are the only ones across that line, each recorded with its reason (§4.4). A user hierarchy under a user base (`class AppError: Exception`, `class NotFound: AppError, IFailure`) stays free to mix kinds: no arm on a class of the documents is affected. The rule is the strict direction on purpose: it can be relaxed later without breaking a program, and the reverse cannot. Q5.

Why not the base class, which needs no rule: requirement 3. The base class cannot hold the two bridged conditions, so either 26 arms and the domain arms flip, or out-of-range input stays a defect and a boundary arm lets "99999999999" through as a bug. It also makes every later re-banding a change of base class. Java drew its checked/unchecked line by base class and has `NumberFormatException` — the plainest input failure — under the unchecked `IllegalArgumentException` for exactly this reason (HYPOTHESIS: prior art from memory, no source consulted).

Why no members on the marker: with the binding an `Exception`, a member on the marker is a second way to reach what the binding already has.

### 3.4 Sendability, FFI, purity

- **Sendability.** No exception class is sendable (§30.14), and implementing the marker changes nothing: a `sealed` class implementing it is still rejected as a spawn argument (f17). The two decline rows on the point stand (§8).
- **FFI.** Exceptions do not cross the boundary; one that escapes a callback aborts the program (§32.8, QUOTED in §6.5). The kind does not change that.
- **Purity and trivial disposal.** An interface with no members adds no field and no destructor, so every built-in stays trivial-dispose and the pure-throw carve-out (§22.15) is untouched. ▲`ParseRangeException` declares `pure` constructors so that `Parse`, which is `pure`, may raise it (S8, L7).

## 4. The taxonomy

### 4.1 The test: where the condition comes from

**A condition is a failure when it comes from outside the program, and a defect when it comes from the program itself** (text: L3 in §10).

- *Outside*: the **environment** produces it (a missing file, a failed write, the end of an input stream); it is found in **data** in an external form that a member is reading into program values (text that does not parse, bytes that do not decode, a document that does not deserialize); a **peer** produces it (a closed channel, a failure in another domain); a **limit** does (a deadline, a full buffer). A correct program can meet every one of these.
- *The program*: it is a fact about an **argument** the program passed (an index, a key, a count, a radix, a format specifier); about the **state** of an object or collection the program holds (empty, key absent, scope closed, enumeration live); or about its own **arithmetic**.

Each of the 116 conditions in §11 carries its origin, and the table's check fails if a row's kind does not follow from it (MEASURED: 0 such rows).

**The kind does not depend on the tier.** Revision 1 said the throwing form of a query is a defect "because a query stands beside it". That was incoherent, and the critique showed where: `Parse` has `TryParse` beside it, `Console.ReadKey` has `ReadKeyOrNull`, and the proposal ruled both failures; and the normative text itself calls those throwing forms bugs. Three glosses say so (QUOTED):

- stdlib §1, the no-dual rule: "the assert-shaped throwing access ("failure is a bug at this call site") … the §9.1 `Parse`/`TryParse` pair being the model";
- stdlib §1, the Try convention: "the **throwing form** (`Parse`, the `[key]` indexer) treats absence as a bug";
- stdlib §2.1: "`ReadKey` remains the assert-shaped throwing tier — end of input is a bug at its call sites";

and Guide 6 §2 builds on them: calling `int.Parse` on boundary input "re-asserts, in exception spelling, the same misclassification the bad `requires` made".

These glosses cannot stand beside a type-level kind. `FormatException` is the class Guide 6's own `Listen` throws for bad boundary input, and it is the class `int.Parse` raises; one class has one kind. So the proposal separates two things the glosses ran together:

- the **tier** is the *caller's stance*: the throwing form handles nothing at the call and lets an exception propagate; the query form handles the outcome at the call;
- the **kind** is the *condition's origin*, and says what the propagating exception reports.

L21–L23, G6-3 and C1 rewrite the glosses accordingly: the throwing `Parse` raises a failure, because the text is the outside's; the throwing indexer raises a defect, because the dictionary is the program's. The names "assert-shaped" and "branch-shaped" stay. This is a change of recorded doctrine, small but real: after it, `int.Parse(untrusted)` with the failure handled at a boundary is a correct program, where Guide 6 calls it a misclassification today. The routing the guides teach is otherwise kept: a failure the caller *branches on* still goes to `ParseOrNull`, `TryParse` or a `choice` (band 2 and band 3); only the failure *consumed at a boundary* is concerned, and band 4 already assigns that to an exception. Q6.

**Why `dict[key]` and `First()` are defects.** Not because a query exists, but because the program owns the state being tested. The dictionary and the key are both values the program holds, and whether the key is present was decided by the program's own earlier calls. The same holds for an empty collection at `Pop`, an index against a length, a second enumeration of a spent query. The program can know the answer without leaving its own memory, and when it does not know, asking is one call. For text, a file or a peer the answer is not in the program's memory at all. This holds whatever the values were computed from: a key read from user input and looked up with the indexer is still the program asserting something about its own dictionary.

**Arithmetic on external data.** `xs.Sum()` over parsed input, or `start.AddDays(n)` with `n` from a request, overflows only for some inputs, so the condition is data-dependent. The proposal rules it a defect, and the argument should be weighed, not assumed.

- *For a defect.* The operands are typed values the program already holds: they crossed the boundary when they were parsed, and a failure was available there. The overflow is produced by the program's choice of operation and width. "Data-dependent" is equally true of an index or a key computed from input, and those are defects. Rust, Swift and Midori all put overflow on the bug side (HYPOTHESIS: from memory).
- *For a failure.* The stdlib offers no query for arithmetic (no checked add that returns null; MEASURED by grep), so a program that must survive overflow of untrusted magnitudes can only validate ranges first or catch `OverflowException` by name. And v0.292 recorded the `DateTime` range fault as band 4 on the ground that it is "a data-dependent overflow" (QUOTED, stdlib §2.5).
- *The ruling.* A defect. v0.292's ground was about blame — the condition depends on receiver and argument together, so an `Argument*` class misdescribes it — and it is true of every arithmetic overflow, including `a * b`, which no one proposes as a failure. Making `DateTime.Add*` a failure would put it in a different kind from `TimeSpan` arithmetic and from the integer operators that the same sentence calls its class. The consequence is stated plainly: a boundary's `IFailure` arm does not take an overflow computed from request data; the program validates the range where the data enters, or names `OverflowException`. This reverses v0.292's band label (§8). Q7.

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
- `ChannelClosedException` and `ChannelFullException` are failures: the peer closed, or the buffer is full under load.
- `TimeoutException` has no producer in the landed library (stdlib §6.1, QUOTED: "provided for user libraries and future modules"); it is classified for those.

### 4.3 The conditions

`data/taxonomy.tsv` has one row for each (section, exception type) the standard library names, 135 rows; `data/taxonomy_check.py` fails if the document names a pair the table lacks, or if a row's kind does not follow from its origin (MEASURED: 268 mentions, 130 pairs, 57 sections, 25 types, all covered). The 116 rows where a member raises are §11. In summary:

| | Rows |
|---|---|
| defect conditions (argument 43, state 33, arithmetic 7) | 83 |
| failure conditions (data 16, environment 7, peer 1, limit 1) | 25 |
| cancellation | 6 |
| a domain join (kind carried from the original) | 2 |
| **change needed** | |
| failure condition raised today with a defect class → ▲`ParseRangeException` (stdlib §2.3, §9.1, §9.2, §9.3) | 4 |
| join → ▲`DomainFailedException` when the original was a failure (stdlib §6.8, §6.12) | 2 |
| defect condition raised with a failure class → recorded misfit (stdlib §2.6, §2.14, §2.15, §2.18, §3.1, §3.2) | 6 |
| band label reversed, type unchanged (stdlib §2.5) | 1 |

The note priced the straddlers at "48 type-and-section pairs" for three types; this extraction, which also reads code comments, finds 52 for the same three (`ArgumentException` 21, `InvalidOperationException` 22, `OverflowException` 9), and all 52 are read and ruled in the table.

### 4.4 The straddlers, ruled

1. **Out-of-range input to `Parse` and `Convert.To*`** (`OverflowException` today; stdlib §2.3, §9.1–§9.3): a failure; the origin is data being read in. The rationale already says so: "input that spells a number the type cannot hold is a data problem" (QUOTED, rationale §14.9). The members raise ▲`ParseRangeException`, which derives from `OverflowException` and implements the marker (L5–L10). An `OverflowException` arm around a `Parse` call matches as before; an `IFailure` arm now takes bad input and still passes an arithmetic overflow. `TryParse` and `ParseOrNull` are defined as "exactly where `Parse` raises" and are unchanged.
2. **A failure leaving a spawned domain** (`DomainFaultedException` today): the join raises ▲`DomainFailedException`, which derives from `DomainFaultedException` and implements the marker (§6.3).
3. **`Console.ReadKey` at end of input** (`EndOfStreamException`; stdlib §2.1): a failure, by origin (the environment's input stream), raised with a failure class. It is not a misfit; the sentence that calls it "a bug at its call sites" is rewritten (L23).
4. **Six misfits: a defect condition raised with a failure class.** An unrecognized format specifier (`FormatException`; stdlib §2.18, §2.6, §3.1) — the specifier is the program's argument. The write-side serialization conditions (`SerializationException`; stdlib §2.14, §2.15) — an unregistered run-time type, a `Weak` reference to a target never strongly reached, a non-finite float on the JSON write path. A code point the chosen encoding cannot represent, at `Encoding.GetBytes` (`FormatException`; stdlib §3.2) — the same shape as the JSON float, which stdlib §2.15 already calls "a caller-chose-an-unrepresentable-format condition" (QUOTED). `FormatException` and `SerializationException` must be failures, because input is what they are mostly raised for. Each misfit is flagged where it stands under stdlib §1's own sentence ("a landed spelling the rule would place in a different band is a recorded misfit — flagged where it stands, moved only by its own graduation ruling", QUOTED; L19, L20, L24) and none is moved here.

   **Flagging is not a cheap deferral.** A kind is inherited and cannot be shed, so a misfit can only be fixed by raising the condition with a class *outside* the failure class's subtree, and every arm that names the failure class stops taking it. That is true today and stays true; the cost only grows with the code written against the current class. For the specifier it is at least 36 arms in 9 test files now (MEASURED). So the real choice is between re-typing in this landing, while there are no external users, and keeping the misfit for good. What keeping it costs: a boundary's `IFailure` arm takes a bad specifier, or a write of an unregistered type, as if it were bad input. Each of the six fails on every execution of its line or on the first value of its kind, so each shows up in a first test run, which bounds the harm. Q10.
5. **`DateTime.Add*`, `Subtract(span)`, `FromTicks` out of range** (`OverflowException`; stdlib §2.5). The class is right and stays. The section's band label is **reversed**: v0.292 recorded the condition as "a band-4 catchable fault, not a band-1 argument defect" (QUOTED); L4 makes it "a band-1 defect of the arithmetic domain … raised with the arithmetic class and not with an argument-blaming one". §4.1 gives the argument and §8 records the reversal.
6. **Every `InvalidOperationException` and `ArgumentException` condition in the library** (43 pairs): a defect, each a fact about an argument or about state the program holds. No change.

**The library's rule about its own shape** (L3): one class is of one kind; a member does not raise a failure with a defect class or the reverse; no failure class derives from a defect class, except the two bridging classes of items 1 and 2, each recorded with its reason. Stdlib §4.8 keeps `KeyNotFoundException` out from under `InvalidOperationException` "so that a state-fault catch arm does not silently swallow missing-key lookups" (QUOTED). That decline was about a derivation that would make arms *newly* take a type. The two bridges keep arms taking what they take today, and §3.3's second restriction keeps user code from adding a third.

### 4.5 User exception classes

A user class is a defect until it lists the marker: `class ConfigError(message: string): Exception(message), IFailure` (f19). MEASURED: of 1,187 exception class declarations in the corpora, 6 derive from a failure type and would be failures by inheritance; the rest are defects until they opt in. HYPOTHESIS: this is the right default. The other default lets a boundary arm swallow whatever a library author forgot to classify, which is the problem this proposal exists to remove. Q12.

### 4.6 "One mechanism per fault condition"

The no-dual rule keeps its content; its gloss of the two tiers is rewritten (§4.1). What is added is a second, type-level statement: one *kind* per exception class, and the band fixes the kind (L1–L3).

## 5. The boundary arm

### 5.1 Dispatch

No dispatch rule changes. `IFailure => …` is an interface arm; it "matches exactly the throwables whose class implements the interface" (QUOTED, §31.2.1). MEASURED on r40 with a program-declared marker (f13): the arm takes a marked class and a marked subclass of `OverflowException`, and passes a contract violation, an index fault, a null fault and a cancellation.

### 5.2 The binding

`var f: IFailure => …` binds `f` as an `Exception` (S3–S5). In a binding alternative an interface counts as `Exception` when the nearest common base is computed, which is what r40 does (f06). A type parameter keeps its class bound: `where TEx: Exception, IFailure` (f07).

**Reaching an interface's own members.** The binding no longer has them. Inside the arm, `var r := ex as IRetryable;` gives a nullable borrow at the interface type and leaves `ex` an `Exception`, so both surfaces are in reach (MEASURED, f21). The guard form `var ex: Exception when ex is IRetryable` is not a substitute: §12.7 retypes the binding for the arm, which then sees the interface's members and loses `Message` (MEASURED: rejected on r40, f22). Revision 1 offered the guard form as the remedy; that was wrong. MEASURED: the corpora have 5 interface-typed arms, 3 of them binding; one, rule case `s31_2_1_0007`, calls an interface member through the binding and has its program rewritten (§9.4).

**A difference from `case`.** Rationale §11.3 says "`catch` arms mirror `case` arms in every detail that matters", and in a `case` "The binding has the pattern's written type" (QUOTED, §19.3). So `case e of var f: IFailure => …` binds an `IFailure`, and a `catch` arm over the same pattern would now bind an `Exception`. The two already differ for the multi-type alternative, whose binding is the nearest common base and not a written type; §20's rule for the `if` expression notes that difference ("deliberately unlike the nearest-common-base rule of §31.2.1, which applies only to multi-type catch bindings", QUOTED). The ground is the same in both places: a `case` scrutinee may be of any type, a caught value is known to be an `Exception`, and a handler's work is done on the exception. Keeping the mirror exact would leave the marker arm unable to log or wrap, which is what the arm is for. The divergence is stated in the proposed spec text (S5) and recorded in the proposed rationale (R1). Q4.

### 5.3 Ordering, guards, re-raise

All unchanged, and all measured on r40 (f05):

- Arms are tried in order; a class arm written before the marker arm wins for its class.
- A guard on a marker arm declines as any guard does.
- A bare re-raise from a marker arm keeps the exception's class. `throw f` with a marker-typed operand is rejected (f16), as §31.1 says of every interface-typed operand; under the binding rule `f` is an `Exception`, which §31.1 still rejects as a borrowed operand. The re-raise is `throw`.
- The unreachable-arm warning already covers the marker: an arm is unreachable when each type it names is "a class implementing an interface type the earlier arm names" (QUOTED, §31.2.3). After `IFailure => …`, an arm on `FormatException` is unreachable. (r40 emits no warning there, f05 case 4: a compiler gap that predates this proposal.)
- Deliberate suppression gains a narrower form: `IFailure => ()` suppresses failures and lets defects pass (S7; f19).

### 5.4 The catch-all

An `Exception` arm stays the catch-all, with the meaning §31.2.3 gives it, and this proposal adds no diagnostic to it. Defects are "catchable on purpose" (QUOTED, proposal 0076 §6): a test's oracle, a server that must keep serving, the logger of last resort.

**What changes is what the documents teach.** A boundary names `IFailure`; an `Exception` arm belongs after it, in a supervisor that reports the bug; one arm for both is the habit to drop (G2-3, C8, T1). Two examples in teaching position are rewritten to the marker (S6: §31.2.4; G2-4: Guide 2 §5). The other catch-all arms in the documents re-raise, are compile-error examples, or isolate a handler on purpose, and are left (§9.2).

**The warning is designed and deferred.** Revision 1 proposed a compiler warning for an undivided catch-all. The critique recommended deferring it to its own proposal, and on weighing it that is this revision's recommendation too:

- its evidence is test-corpus oracles and the documents' own examples; the 40 everyday programs have no catch-all at all, and this project's gates decline to act on that class of evidence (proposal 0061 §2; proposal 0076 §0);
- nothing else in the proposal depends on it;
- it is non-breaking to add later, and once the marker exists, whether programs keep writing the undivided arm becomes observable;
- its first design had a hole (it exempted `Exception => ()`, the widest swallow, while warning on an arm that logs), which says the rule needs its own review.

§12 records the corrected design, its measured reach and the trigger for proposing it. Q11 puts the choice to the owner.

### 5.5 Idioms

The three shapes the guide teaches (text: G2-3 in §10; MEASURED to compile and run on r40 with a program-declared marker, f19):

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

The kind is read by a catch arm's type test and by a domain join, and by nothing else. In particular:

- **The throw.** "`throw x` is a source-visible kill — the keyword itself is the announcement ([K3.13])" and the operand's ownership "transfers to the runtime's exception machinery, which disposes the exception after the matching catch arm completes" (QUOTED, §31.1). Neither sentence mentions the operand's class beyond its being `Exception`-derived.
- **The edge.** "The exception edge performs the same implicit disposal as the normal edge, on the single establishment-ordered cleanup stack" (QUOTED, K3.15); §28.3's exception-path discharge derives from it. MEASURED (f12): a failure edge and a defect edge run `close b, finally, close a` identically.
- **A consumed argument.** "A raise from the consuming call does not hand the obligation back" (QUOTED, §28.8.1), for either kind.
- **Dispose-after-arm.** A failure that owns a payload is disposed after a bare marker arm with no binding (MEASURED, f12).
- **The caught borrow.** A binding typed `Exception` under §5.2 is the same borrow of the in-flight exception; §31.1's escape rules apply as written.

Kernel classification ([`spec-change-process.md`](spec-change-process.md) §9): no K rule, axiom or register row is touched. Body-locality (proposal 0105 §1) of the new compile-time rules: the binding rule and the two conformance restrictions are each **local** (the restrictions read the declaration's own base chain).

### 6.2 Async

A task's fault is the exception instance; its class, and so its kind, rides it. MEASURED (f11): `await` in a `try` whose arm is the marker takes a failure and passes a bug; two awaiters of one faulted task both see the marker; the frame's `using var` is disposed on both edges.

- `IsFaulted` means "completed with an exception", of either kind. No member is added to tell them apart; the task's exception is reachable only by awaiting (§30.9), and the arm does the telling.
- `Task.WhenAll` "re-raises the exception of the first non-successful input in input order" (QUOTED, §30.10). So a failure in the first input is re-raised while a bug in the second stays observable only on that task (MEASURED, f11 case 4). This is today's behaviour and is not changed; §14 records it and Q13 asks whether the owner wants more.
- `TaskScope`, `WhenEach`, `FromException`, `SetException`: the kind is the class of the exception they carry. No text changes.

### 6.3 Domains

"An exception that propagates out of the entry does not cross the boundary as an object (no exception class is sendable …)" (QUOTED, §30.14). That stays. One fact about the original crosses with the two strings: whether its class implements the marker. The join raises ▲`DomainFailedException` when it does and `DomainFaultedException` when it does not (S12, L15–L17).

- A class's interfaces are fixed, so one class cannot be "a failure when the original was". Hence a subclass. MEASURED (f10): a class of that shape, declared by hand, is taken by a marker arm and by an existing `DomainFaultedException` arm; the plain class is taken by the second only. (Under §3.3's second restriction only the library may declare that shape.)
- Cancellation leaving an entry raises the base class, so a marker arm at a join never takes a cancelled child.
- The kind composes across nested spawns: a `DomainFailedException` that leaves an entry implements the marker, so the outer join raises `DomainFailedException` again.
- `Parallel.*` uses the same vocabulary (S13, L17).
- Proposal 0110 (under review) would add an `ExitKind` of `Fault` or `Cancelled` to `DomainFaultedException`. The two are orthogonal: 0110's property tells a fault from a cancellation; this class tells a failure from a defect among faults. If both land, `DomainFailedException` always has `ExitKind = Fault`.

### 6.4 Contracts and purity

`ContractViolationException` is a defect and never implements the marker; a clause "states a proposition" and its violation is "a bug report, not a result" (QUOTED, §22.16). MEASURED (f13): a `requires` violation passes a marker arm. `requires` remains wrong for input whose invalidity is a normal outcome; that input is a failure, raised by the body (G6-1 to G6-4).

### 6.5 FFI

"Exceptions do not cross the boundary. An exception escaping a Bezel callback reaches the FFI boundary and **aborts the program**" (QUOTED, §32.8). The rule is the same for both kinds: native frames cannot be unwound for a failure any more than for a bug. A wrapper that turns `errno` into an exception (§32.9) raises a failure class, normally one of the `IOException` family. This could not be probed: on r40 a native callback segfaults even for rule case `s32_8_0021`'s own program (§14).

### 6.6 Supersession and cause chains

Later-wins is unchanged (§31.2; rationale §11.8). It has one consequence under the marker that must be stated: a `finally` or a disposer that raises a *failure* while a *defect* is propagating replaces the defect, and a marker arm then takes the cleanup failure (MEASURED, f14). The defect is not lost: it is the first entry of the failure's `SupersededChain` (§31.4) and `ToString()` renders it. A boundary that must not answer such a failure as an ordinary one tests the chain in a guard — `var f: IFailure when f.SupersededChain.Count = 0 => …` — and the superseding failure falls to the next arm. No rule is proposed. The reopening bar rationale §11.8 states for primary-wins ("arm matching on the primary is the one gain the snapshot does not provide", QUOTED) describes this case; the marker makes it more visible and supplies no field evidence, so the bar is not met.

Cause chaining copies strings; the wrapper's kind is its own class's. The guide's rule is: wrap a failure in a failure; do not wrap a defect, re-raise it. `ExceptionInfo` is not given a kind field: it "is a record of `string`s" (QUOTED, §31.2) whose widening was declined once already (§31.4), and the entry's `TypeName` identifies it in a log.

### 6.7 What does not change, in one list

`throw` as an expression; `finally` ordering; the pure-throw carve-out; `Task.FireAndForget`'s logging; the uncaught-exception report and exit code 70 (§33.3), for both kinds; `yield` restrictions around `try`; the absence of a `throws` clause (§31.3); every diagnostic on a `catch` arm.

## 7. Compatibility with the deferred `(try E)`

The note defines the atom as: evaluate `E`; "If evaluation of `E` raises an exception whose class implements the marker, handling of that exception completes at the atom … and the atom's value is `null`. Any other exception propagates" (QUOTED, note §8.1). Everything it needs from this proposal is the predicate "the class implements `IFailure`", decidable at run time from the object. This proposal supplies that predicate and nothing more.

- The rulings of §4 are the ones the atom would inherit: `(try int.Parse(s))` would yield `null` for out-of-range text and `(try list.First())` would propagate. No ruling here was bent toward the atom; each is justified by the origin test alone.
- Cancellation is not a failure in any subclass (§3.3), so the atom would never swallow it.
- ▲`DomainFailedException` makes `(try await join)` meaningful without further design.
- Nothing here is defined in terms of the atom, and no text in §10 would need rewording if the atom never lands.

The atom's own gate (note §8.5) is untouched: this proposal provides none of its four conditions.

## 8. Recorded declines and gates

**No declined alternative is re-proposed.** But the proposal is not free of contact with the record. It changes two recorded positions and diverges from one recorded statement, and each needs the owner's ruling:

1. **It reverses a recorded classification.** v0.292 classified the `DateTime` range fault as band 4 ("a band-4 catchable fault, not a band-1 argument defect", stdlib §2.5; the decline index row *`ArgumentOutOfRangeException` as the `DateTime` … range fault* records it as "re-classified … to band 4's catchable fault", QUOTED). L4 makes it band 1. The declined alternative of that row (`ArgumentOutOfRangeException`) stays declined, and the class raised stays `OverflowException`. §4.1 argues the reversal; Q7.
2. **It rewrites a recorded gloss.** The throwing tier as "a bug at this call site" (stdlib §1 twice, stdlib §2.1; taught in Guide 6 §2) becomes "the call site handles nothing", with the kind left to the condition (§4.1). Q6.
3. **It diverges from a recorded statement.** Rationale §11.3's "`catch` arms mirror `case` arms" and §19.3's "The binding has the pattern's written type" (§5.2). Q4.

The other rows read, by their index name in `DECLINED.md`:

| Row | Effect |
|---|---|
| *Value-form try expression* (D-0005); proposal 0076 §7 ("Untouched and *reinforced* — on both grounds") | **Untouched as a decline.** No expression form is added. One of its grounds is touched at the edge by item 2 above: the ground says exception-based expected failure is the path Guide 2 routes to value shapes; after the gloss rewrite, an expected failure consumed *at a boundary* is openly an exception (band 4 already said so), and one the caller branches on is still routed to a value. Admission test 3 of proposal 0061 ("does not make a doctrine-declined path more ergonomic") is not engaged: no interior call site gets easier. |
| *`Result`-first error model*; proposal 0076 §6's four-condition bar | **Untouched.** None of the four conditions is claimed or needed. |
| *Information content as the failure-band test* | **Untouched.** The test of §4.1 is the condition's origin, not how much the exception carries. |
| *`KeyNotFoundException` deriving from `InvalidOperationException`* | **Untouched; its reasoning is applied**, to the library (§4.4) and to user classes (§3.3). |
| *Checked enum conversion — a dedicated fault subclass rather than `OverflowException`* ("no plausible handler branches enum-cast-vs-overflow") | **Untouched.** ▲`ParseRangeException` is a subclass for a different condition, and it meets the row's own bar: a handler that branches on it exists, the boundary arm. |
| *`NotSupportedException`*; "the taxonomy stays deliberately small" | **Untouched.** Two classes are added, each because a handler selects on it. |
| *`Exception`-derived-throwable alternatives* (proposal 0080); *Non-`Exception` throwability* (retired) | **Built on.** Interface arms were kept admissible there on purpose; the binding rule extends §31.2.1's nearest-common-base rule. |
| *Modelling which `catch` arm a raise can reach by exception type* (proposal 0198 §7) | **Untouched.** Nothing here reads a raise. |
| *Primary-wins exception supersession* | **Untouched** (§6.6). |
| *Sendability requirement on throwable types*; *Stating `Exception`'s inherited state transfer-compatible* | **Untouched** (§3.4, §6.3). One bit crosses a join as the class of the exception the join raises. |
| *Forcible domain teardown*; *Catchable allocation failure*; *`defer`/`errdefer`*; *`TextReader.Read()` returning `char?`*; *Enum name-parse tiers*; *Per-site suppression spellings for the statically-certain-hazard lints* | **Untouched.** |
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
| user exception classes that become failures without being edited | 6 of 1,187 |
| newly warned | nothing |

Newly rejected: a type that lists the marker without deriving from `Exception`, and a class that lists it under a non-failure class of the documents. The test corpora contain neither (they do not name the marker).

### 9.2 Documents: the audit

`data/doc_sites.py` reads the eleven documents the docs gate covers and the Tour, finds every line in six groups, and fails unless each is either changed by an edit of §10 or listed with a reason for leaving it (MEASURED: 71 sites, 0 unaccounted; `data/doc_sites.tsv`).

| Group | Sites | Edited | Left, and why |
|---|---|---|---|
| a gloss of the throwing tier as "a bug" or "assert-shaped" | 14 | 5 (L21, L22, L23, G6-3, C1) | 9: each is about a cast, an index, a key or an empty collection, which are the program's, so the gloss is still right (spec, two enum-cast lines; stdlib §4.3, §4.5, §9.9; Guide 5 three times), and one rationale line is not about exceptions |
| a mention of `DomainFaultedException` | 18 | 9 (S12, S13, L15–L17, R2, G4-1, G4-2, C3) | 9: true as written, the new class being a `DomainFaultedException` (two *Async entries* paragraphs, two example comments, the declaration and the sentence that introduces it, the cross-reference in stdlib §2.17, one rationale line, the reference's `Parallel` line) |
| `OverflowException` on a line that names `Parse` or `Convert` | 14 | 10 (L4–L10, G6-3, C5, C6) | 4: a cast (stdlib §2.12); the rationale's `double.Parse` range record, three lines, true of the subclass |
| the phrase "`FormatException`/`OverflowException`" | 2 | 0 | 2: the contract's name in stdlib §9.1's listing; true of the subclass, and L7 says so |
| a catch arm naming `Exception` alone, in a fenced block | 17 | 2 (S6, G2-4) | 15: nine re-raise; two are compile-error examples; §31.2.5's suppression; §31.1's closure example, whose subject is the closure; and two deliberate handler isolations (stdlib §6.5, rationale §13.5) |
| "boundary fault" / "defect exception" / "format fault" | 6 | 5 (L1; G2-1, two lines; G2-2; G6-2) | 1: Guide 6's summary of band 1, consistent |

The other 34 edits are found by no group, because their text is new or their line names something else: the marker, who may list it and where its name is visible (S1, S2, S10, S14, S15); the binding rule (S3–S5); `IFailure => ()` (S7); the purity sentence, the tree line and the list of stdlib classes (S8, S9, S11); band 4 and the kinds paragraph (L2, L3); the four declarations (L11–L14); the pointer in stdlib §2.17 (L18); the three misfit flags (L19, L20, L24); the rationale section and its pointer (R1, R3); Guide 2's new passage, table rows and closing sentence (G2-3, G2-5, G2-6); two of Guide 6's edits (G6-1, G6-4); the Tour (T1); and the reference's `Convert` comment, `Domain` comment and two idioms (C2, C4, C7, C8).

Pins: Guide 2 stands 27 revisions behind today (MEASURED, `check_docs.sh`), so the landing re-traces all of it; Guides 4 and 6, the Tour and the reference move their pins in the same commit ([`spec-change-process.md`](spec-change-process.md) §9).

### 9.3 Conformance cases to add at landing

Each case states the verdict the proposed text requires. *Cites* names the edit of §10 whose sentence the case quotes.

| # | Section | Expect | Program | Cites |
|---|---|---|---|---|
| 1 | §31.4 | run | a user class listing the marker is thrown; an `IFailure` arm takes it | S10 "an `IFailure` arm takes every failure" |
| 2 | §31.4 | run | `InvalidOperationException` passes an `IFailure` arm and reaches an outer `Exception` arm | S10 "lets defects and cancellation pass" |
| 3 | §31.4 | run | `OperationCanceledException` passes an `IFailure` arm | S10 "a handler of failures never takes it" |
| 4 | §31.4 | run | `int.Parse("12x")` is taken by an `IFailure` arm | S10 "`FormatException` implements `IFailure`" |
| 5 | §31.4 | run | a class deriving from a marked class, not listing the marker, is taken | S10 "a kind is inherited and cannot be shed" |
| 6 | §31.4 | reject | a record lists `IFailure` | S10 "Only an `Exception`-derived class" |
| 7 | §31.4 | reject | a class outside the `Exception` family lists `IFailure` | the same |
| 8 | §31.4 | reject | a record implements an interface that extends `IFailure` | S10 "both restrictions then bind its implementers" |
| 9 | §31.4 | accept | an interface extends `IFailure`; a class under `Exception` implements it | the same |
| 10 | §31.4 | reject | `class C: OperationCanceledException, IFailure` | S10 "is cancellation in every subclass" |
| 11 | §31.4 | reject | `class S: InvalidOperationException, IFailure` | S10 "never takes a failure that user code declared" |
| 12 | §31.4 | reject | `class T: S, IFailure` over a user `class S: ArgumentException` | S10 "where a class's base chain contains" |
| 13 | §31.4 | run | `class AppError: Exception`; `class NotFound: AppError, IFailure`; an `IFailure` arm takes `NotFound` and passes `AppError` | S10 "chooses freely at each level" |
| 14 | §31.2.1 | run | `var f: IFailure => Console.WriteLine(f.Message)` prints the message | S5 "binds `name` at `Exception`" |
| 15 | §31.2.1 | accept | `var f: IFailure => throw new Wrapped("x", f)` | S5 "chains as a cause" |
| 16 | §31.2.1 | run | `var ex: InvalidOperationException \| IFailure` reads `Message` | S4 "an interface counting as `Exception`" |
| 17 | §31.2.1 | reject | `var r: IRetryable => r.ShouldRetry()` | S5 "It does not type a binding" |
| 18 | §31.2.1 | run | `var ex: IRetryable => do var r := ex as IRetryable; …` calls `r!.ShouldRetry()` and reads `ex.Message` | S5 "gives a nullable borrow at the interface type and leaves `ex` an `Exception`" |
| 19 | §31.2.1 | reject | `var ex: Exception when ex is IRetryable => ex.Message` | S5 "no longer the `Exception` surface"; §12.7 |
| 20 | §31.2.1 | reject | an arm over `TEx` with `where TEx: IFailure` alone | S5 "never the interface alone" |
| 21 | §31.2.3 | warn | `IFailure => …` then `FormatException => …` (unreachable arm; existing rule) | §31.2.3 "a class implementing an interface type the earlier arm names" |
| 22 | §31.2.5 | run | `IFailure => ()` suppresses a failure; a defect in the same block propagates | S7 |
| 23 | §30.14 | run | a child raises a marked class; an `IFailure` arm at the join takes it | S12 "the join raises `DomainFailedException`" |
| 24 | §30.14 | run | a child raises `InvalidOperationException`; an `IFailure` arm passes it, a `DomainFaultedException` arm takes it | S12 "An arm naming `DomainFaultedException` takes both" |
| 25 | §30.14 | run | a child raises a marked class; a `DomainFaultedException` arm takes it and reads the copied `Message` | the same |
| 26 | stdlib §6.12 | run | a `Parallel.Map` body raises a failure; an `IFailure` arm takes it at the `await` | L17 |
| 27 | stdlib §9.1 | run | `int.Parse("99999999999")` is taken by an `IFailure` arm | L7 "raises `ParseRangeException`" |
| 28 | stdlib §9.1 | run | an arithmetic overflow passes an `IFailure` arm | L7 "no arm on `IFailure` takes it" |
| 29 | stdlib §2.3, §9.2, §9.3 | run | `Convert.ToInt32`, `double.Parse`, `decimal.Parse` out of range: one case each, taken by an `IFailure` arm | L5, L9, L10 |
| 30 | stdlib §7.5 | run | opening a missing file is taken by an `IFailure` arm | L14 |
| 31 | stdlib §6.9 | run | `Send` after `Close`, and `Send` on a full bounded channel: one case each | L13 |
| 32 | stdlib §2.15 | run | malformed JSON is taken by an `IFailure` arm | L11 |
| 33 | stdlib §1 | run | `dict[missing]` and `xs.First()` on an empty array each pass an `IFailure` arm | L3 "the indexer raises a defect, because the dictionary is the program's" |
| 34 | stdlib §2.5 | run | `DateTime.MaxValue.AddDays(1)` passes an `IFailure` arm and is taken by an `OverflowException` arm | L4 |
| 35 | §30.9 | run | a task faulted with a failure: an `IFailure` arm takes it at each of two awaits | S10 "a task's fault" |
| 36 | §31.2 | run | a `finally` raises a failure while a defect propagates; the `IFailure` arm takes it and `SupersededChain.Count` is 1 | §31.2 (unchanged), S10 |
| 37 | §22.16 | run | a `requires` violation passes an `IFailure` arm | S10 |

Cases 6–20 depend on rules the compiler does not implement; they enter the baseline as `fail` rows, which is the corpus's design.

### 9.4 Rule-corpus sweep (spec-change-process §5 item 3c)

One existing case's program changes: `s31_2_1_0007` ("an interface-typed catch arm matches an implementing exception") binds `var ex: IRetryable` and calls `ex.ShouldRetry()`. Its rule sentence and its `run` verdict stay; its program is rewritten to reach the member through `as` inside the arm, and cases 17 and 18 above pin the two halves. `l9_1_0007` (`int.Parse` raises `OverflowException` on an out-of-range value) and `l6_8_0003` (a fault surfaces as `DomainFaultedException`) keep their verdicts, since each arm still matches; their titles and `rule:` headers are re-read at landing. The gtest `binding_no_shared_base_types_as_exception_spec_31_2_1_v0_243` already pins the alternative half of the binding rule.

### 9.5 Change class per part

| Part | Class | Why |
|---|---|---|
| the marker; the two conformance restrictions | design change | new built-in surface; two new rejections |
| the catch-binding rule | design change for the single-interface binding (a settled typing changes; a recorded mirror is diverged from); a ruling for the alternative (the text did not cover an interface; the compiler's reading is stated) | |
| the kind of each existing type; the origin test; the revised failure-signal rule and tier glosses | design change | a new normative classification of landed types, and a recorded gloss rewritten |
| ▲`ParseRangeException` at `Parse`/`Convert` | design change | the class a member raises changes; every existing arm keeps matching |
| ▲`DomainFailedException` at joins | design change | the same |
| the six misfit flags | ruling | records a classification; no behaviour changes |
| the stdlib §2.5 label | design change | reverses a recorded classification (§8) |
| the Appendix E row | ruling | classifies a delta the appendix did not name |
| the guides, the Tour, the reference | guide revalidation riding the train | |
| **ratchet** | none | no warning is proposed |

All parts are meant to land as one revision after acceptance: the marker without the taxonomy selects nothing, and the taxonomy without the binding rule gives a boundary arm that cannot log.

### 9.6 Trackers to file at landing (`spec-tracking`)

Each is a spec-ahead-of-implementation delta: (1) the built-in `IFailure` and the conformances of the ten landed failure types; (2) the single-interface binding typed as `Exception`; (3) the two conformance restrictions; (4) `ParseRangeException` and its four raise sites; (5) `DomainFailedException` at `Domain.Spawn` joins and `Parallel`. Compiler gaps met on the way, which predate the proposal, are in §13.

### 9.7 Decline-index rows to add at graduation

`declined-at-proposal`, each pointing at this proposal's section: a base class `Failure: Exception` as the marker (§3.3; bar: evidence that the binding rule misleads readers, with a migration for the two bridges); reporting members on the marker, and an implicit `IFailure`-to-`Exception` conversion (§3.2); the kind as a virtual property, a class modifier, or a throw-site keyword (§3.2); keeping the catch-arm binding at the interface type, as `case` does (§5.2); a user class listing the marker under a defect or cancellation class of the documents (§3.3; bar: a user library with arms to keep matching, as the two bridges had); a default of "unmarked means failure" (§4.5); the kind following the tier, so that a throwing form with a query beside it is a defect (§4.1); arithmetic over external data as a failure (§4.1); a kind field on `ExceptionInfo` (§6.6); abandoning the process or the domain on a defect, and any rule forcing callers to handle failures (the note's candidates D and B). `deferred`: the undivided catch-all warning (§12; bar: its trigger). And one row edited: the `DateTime` range-fault row, whose recorded band changes from 4 to 1 (§8).

### 9.8 The Migration note, in outline

*Newly rejected:* a type that lists `IFailure`, or an interface extending it, and is not an `Exception`-derived class; a class that lists it under a non-failure class of the documents. *Newly defined:* `IFailure`; the kind of every exception type; `ParseRangeException`; `DomainFailedException`; the type of a binding over an interface arm. *Newly warned:* nothing. *Behaviour changes with every arm still matching:* out-of-range `Parse` input raises `ParseRangeException`; a failure leaving a domain is joined as `DomainFailedException`. *Verdict change:* a binding over a single interface arm no longer exposes the interface's members. *Doc-only:* the misfit flags; the tier glosses; the stdlib §2.5 band label. *Spec-ahead-of-impl:* the five trackers of §9.6.

## 10. Proposed text

The edits below are exact. Each `old` text occurs once in its document at v0.355, and the whole set applies in order (MEASURED: `data/edits.py check`, 63 edits over 8 documents, 0 anchor failures). Applied to a scratch clone, the patched documents pass `scripts/check_docs.sh` with every anchor resolving and 558 example fragments extracted, against 553 before (MEASURED). `data/edits.py` is the same set in machine-checkable form, kept so that the anchors can be re-verified at graduation; its `twin` command checks it against this section. The changelog entry, the version headers, the pins and the `DECLINED.md` rows are written at landing and are not in the set.

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
**Interface arm types.** An interface named in an arm is a filter: the arm takes exactly the throwables whose class implements it. It does not type a binding. Every throwable is an instance of an `Exception`-derived class ([§31.1](#sec-the-throw-operand)), and nothing narrower is known of one matched through an interface, so a binding arm over an interface binds `name` at `Exception`, and an interface listed in a binding alternative counts as `Exception` in the nearest-common-base computation: `var f: IFailure` and `var ex: IOException | IRetryable` each bind an `Exception`. The binding therefore reads `Message`, is an argument where an `Exception` is expected, and chains as a cause ([§31.4](#sec-standard-exception-types)), whatever interface selected it. The interface's own members are reached by a type test inside the arm: `var r := ex as IRetryable;` gives a nullable borrow at the interface type and leaves `ex` an `Exception`. The guard form `var ex: Exception when ex is IRetryable` instead retypes the binding for the arm ([§12.7](#sec-type-tests-is-is-not-as)), which then sees the interface's members and no longer the `Exception` surface. This is a difference from `case`, where a type pattern's binding has the pattern's written type ([§19.3](#sec-patterns)): a `case` scrutinee may be of any type, while a caught value is always an `Exception`, and the arm's binding says so. A type parameter used as an arm type keeps its class bound (below): `where TEx: Exception, IFailure`, never the interface alone. The failure marker `IFailure` ([§31.4](#sec-the-failure-marker)) is the interface this rule is written for.
````

**S6** — §31.2.4, the last example's second arm: replace

````text
  var ex: Exception => Log(ex)
end;
````

with

````text
  var f: IFailure => Log(f)                 // every other failure; `f` is an `Exception` (§31.2.1); defects pass
end;
````

**S7** — §31.2.5, end of the last paragraph: insert after

````text
Silent suppression of exceptions is a code smell, and the syntax does not hide it.
````

the text

````text
 To suppress expected failures only, name the marker — `IFailure => ()` — and defects and cancellation still propagate ([§31.4](#sec-the-failure-marker)).
````

**S8** — §31.4, the first paragraph: replace

````text
(and `ArgumentException`/`ArgumentOutOfRangeException`'s `(message, paramName)` forms) are **`pure`**
````

with

````text
(and `ArgumentException`/`ArgumentOutOfRangeException`'s `(message, paramName)` forms, and the `()` and `(message)` forms of `ParseRangeException`, stdlib [§9.1](Bezel_Standard_Library.md#sec-integer-types)) are **`pure`**
````

**S9** — §31.4, the hierarchy tree: replace

````text
├─ FormatException                  — string does not conform to an expected format.
````

with

````text
├─ FormatException                  — string does not conform to an expected format. A failure: implements `IFailure`.
````

**S10** — §31.4, new paragraphs after the tree's arm-ordering paragraph: insert after

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

`IFailure` declares no members: it classifies, and the object it classifies is always reachable as an `Exception` ([§31.2.1](#sec-arm-patterns), *Interface arm types*). It is available without import, with the hierarchy above. A class implements it by listing it, and a class deriving from one that implements it implements it too ([§14.2](#sec-interface-conformance) rule 1), so a kind is inherited and cannot be shed.

**Who may conform.** Only an `Exception`-derived class: a record, an enum, a choice, or a class outside the `Exception` family that lists `IFailure`, or an interface extending it, is a compile error at the declaration. No value of such a type can be thrown ([§31.1](#sec-the-throw-operand)), so the conformance could never be observed — [§31.2.1](#sec-arm-patterns)'s ground for rejecting a dead arm type. And a class does not contradict the kind of a class the documents declare: a class that does not inherit `IFailure` may list it only where no class in its base chain, other than `Exception` itself, is a built-in or standard-library exception class; listing it otherwise is a compile error at the declaration. So a class deriving from `OperationCanceledException` is cancellation in every subclass, and an arm naming `InvalidOperationException`, or any other defect class of the documents, never takes a failure that user code declared. A class whose base chain is user-declared down to `Exception` chooses freely at each level. The standard library declares the only two classes that derive across that line, each by recorded ruling (stdlib [§1](Bezel_Standard_Library.md#sec-failure-signal-rule)). An interface may extend `IFailure`; both restrictions then bind its implementers.

Of the built-in types, `FormatException` implements `IFailure`. The standard library's failure types, and the rule by which a condition is raised as a failure or as a defect, are stated in stdlib [§1](Bezel_Standard_Library.md#sec-failure-signal-rule). A class that this specification or the standard library declares changes kind only by recorded ruling. User code chooses per class: `class ConfigError(message: string): Exception(message), IFailure` declares a failure, and a class that does not list the marker declares a defect report.

The kind is read in two places. A catch arm that names the marker selects on it: an `IFailure` arm takes every failure and lets defects and cancellation pass ([§31.2.3](#sec-catch-ordering-and-exhaustiveness)). And a domain join carries it across the boundary ([§30.14](#sec-multiple-domains)). Nothing else reads it. The ownership of a thrown exception ([§31.1](#sec-exception-ownership)), the cleanup an exception edge runs ([K3.15](#sec-kernel-k3)), a task's fault ([§30.9](#sec-exception-handling-in-async-methods)), the purity carve-out ([§22.15](#sec-pure-throw)), supersession ([§31.2](#sec-try-catch-finally)), and the reports of [§32.8](#sec-callbacks) and [§33.3](#sec-exit-code) are the same for both kinds and for cancellation.
````

**S11** — §31.4, the last paragraph: replace

````text
`FileNotFoundException`, `IOException`, and others. Outside the auto-imported modules
````

with

````text
`ParseRangeException` (deriving from `OverflowException` and implementing `IFailure`; raised by the `Parse` family for a value outside the type's range; declared in the auto-imported `System`, stdlib §9.1), `FileNotFoundException`, `IOException`, and others. Outside the auto-imported modules
````

**S12** — §30.14 *Faults*: replace

````text
whose `Message` and `FaultStackTrace` carry copies of the original exception's message and stack-trace text (`string`s — sendable). Programs that need richer fault data
````

with

````text
whose `Message` and `FaultStackTrace` carry copies of the original exception's message and stack-trace text (`string`s — sendable). The exception's kind crosses with them: where the original exception's class implements `IFailure` ([§31.4](#sec-the-failure-marker)), the join raises `DomainFailedException`, which derives from `DomainFaultedException` and implements `IFailure` (stdlib [§6.8](Bezel_Standard_Library.md#sec-domain)). An arm naming `DomainFaultedException` takes both; an arm naming `IFailure` takes a failure raised in another domain as it takes one raised in its own. Programs that need richer fault data
````

**S13** — §30.14 *Data-parallel combinators*, *Results and faults*: replace

````text
the returned task faults with `DomainFaultedException` — this section's fault vocabulary, with the no-aggregation
````

with

````text
the returned task faults with `DomainFaultedException` (`DomainFailedException` where that first fault is a failure) — this section's fault vocabulary, with the no-aggregation
````

**S14** — Appendix C.7, the multi-type binding bullet: replace

````text
- A multi-type catch alternative in binding form types the binding as the listed types' **nearest common base** — with every listed type `Exception`-derived, always `Exception` or a class deriving from it ([§31.2.1](#sec-arm-patterns)).
````

with

````text
- A multi-type catch alternative in binding form types the binding as the listed types' **nearest common base** — with every listed class type `Exception`-derived and an interface counting as `Exception`, always `Exception` or a class deriving from it; a binding arm over one interface type types the binding as `Exception` ([§31.2.1](#sec-arm-patterns)).
- `IFailure` is listed only by a class deriving from `Exception`, or extended by an interface; a record, enum, choice, or class outside the `Exception` family that conforms to it is a compile error at the declaration, and so is a class that does not inherit it and has a built-in or standard-library exception class other than `Exception` in its base chain — `OperationCanceledException` and every defect class among them ([§31.4](#sec-the-failure-marker)).
````

**S15** — Appendix E, a new row after the *public type removed* row: insert after

````text
| public type removed; base class changed; implemented interface removed | **breaking** | An interface *added* to a type: widening. |
````

the text

````text
| `IFailure` added to the interfaces of an exception class | **breaking** | The one exception to *an interface added to a type: widening*. The kind is what a consumer's boundary arm selects on ([§31.4](#sec-the-failure-marker)): adding the marker makes every `IFailure` arm take the class. (Removing it is breaking by the row above.) |
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
**Failures and defects.** The exceptions this library raises are of the kinds language spec [§31.4](Bezel_Language_Spec.md#sec-the-failure-marker) defines, and the band fixes the kind: a band-4 condition is raised with a class that implements `IFailure`, a band-1 condition with one that does not, and cancellation with `OperationCanceledException`. The test between bands 1 and 4 is **where the condition comes from**. It comes from outside the program when the environment produces it (a missing file, a failed write, the end of an input stream), when it is found in data in an external form that a member is reading into program values (text that does not parse, bytes that do not decode, a document that does not deserialize), when a peer produces it (a closed channel, a failure in another domain), or when a limit does (a deadline, a full buffer). A correct program can meet every one of these, and the condition is a failure. It comes from the program itself when it is a fact about what the program passed (an index, a key, a count, a radix, a format specifier), about the state of an object or collection the program holds (an empty collection, an absent key, a closed scope, a live enumeration), or about its own arithmetic. The program owns that state and can know it before the call, so the condition is a defect, however the values in it were arrived at. The kind is the condition's and not the tier's (the no-dual rule, below): `Parse` and the `[key]` indexer are both throwing forms with a query beside them, and `Parse` raises a failure, because the text is the outside's, where the indexer raises a defect, because the dictionary is the program's. The failure types this document declares are `IOException` and the classes deriving from it ([§7.5](#sec-io-exception-types)), `SerializationException` ([§2.15](#sec-concrete-serialization-formats)), `TimeoutException` ([§6.1](#sec-cancellationtoken-and-cancellationtokensource)), `ChannelClosedException` and `ChannelFullException` ([§6.9](#sec-channel)), `ParseRangeException` ([§9.1](#sec-integer-types)) and `DomainFailedException` ([§6.8](#sec-domain)); with the built-in `FormatException` they are the library's whole failure band. One exception class is of one kind, and a member does not raise a failure with a defect class or a defect with a failure class. No class of this library that implements `IFailure` derives from one that does not, with two recorded exceptions, each keeping an established catch arm matching: `ParseRangeException` derives from `OverflowException`, and `DomainFailedException` from `DomainFaultedException`. A landed member that raises a failure class for a band-1 condition is a recorded misfit under this rule's scoping sentence, flagged where it stands: the unrecognized format specifier ([§2.18](#sec-iformattable), [§2.6](#sec-guid), [§3.1](#sec-stringbuilder)), the write-side serialization conditions ([§2.14](#sec-serialization-vocabulary), [§2.15](#sec-concrete-serialization-formats)) and the unrepresentable code point at `GetBytes` ([§3.2](#sec-encoding)). A kind is inherited and cannot be shed (language spec §31.4), so moving such a condition means raising it with a class outside the failure class's subtree, and the arms naming the failure class stop taking it. A class's kind changes only by recorded ruling, as the taxonomy grows only by one (language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)).
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

**L21** — §1, the Try convention, *the three access tiers*: replace

````text
the **throwing form** (`Parse`, the `[key]` indexer) treats absence as a bug, the **`*OrNull` form** returns
````

with

````text
the **throwing form** (`Parse`, the `[key]` indexer) asserts the value is there and raises where it is not (a failure or a defect by where the condition comes from — *Failures and defects*, below), the **`*OrNull` form** returns
````

**L22** — §1, the no-dual rule's gloss of the two tiers: replace

````text
the assert-shaped throwing access ("failure is a bug at this call site") beside the branch-shaped value access ("failure is expected here"), the [§9.1](#sec-integer-types) `Parse`/`TryParse` pair being the model — because tiers serve different call-site *stances* toward one operation, not the same problem twice.
````

with

````text
the assert-shaped throwing access ("this call site handles nothing: it asserts the operation succeeds, and an exception propagates if it does not") beside the branch-shaped value access ("this call site handles the outcome"), the [§9.1](#sec-integer-types) `Parse`/`TryParse` pair being the model — because tiers serve different call-site *stances* toward one operation, not the same problem twice. A stance is the caller's. It does not make the exception a failure or a defect, which is the condition's (*Failures and defects*, above): the throwing `Parse` raises a failure and the throwing indexer a defect.
````

**L23** — §2.1 `Console`, the `ReadKey` paragraph: replace

````text
`ReadKey` remains the assert-shaped throwing tier — end of input is a bug at its call sites.
````

with

````text
`ReadKey` remains the assert-shaped throwing tier: a caller that does not expect end of input handles nothing, and the `EndOfStreamException` then raised is a failure ([§1](#sec-failure-signal-rule)), end of input being the environment's and not the program's.
````

**L24** — §3.2 `Encoding`, the defined-fault paragraph: insert after

````text
`GetBytes` raises `FormatException` when the string contains a code point the encoding cannot represent (possible only for `ASCII`).
````

the text

````text
 (The `GetBytes` half is a band-1 defect raised with a failure class, a recorded misfit under [§1](#sec-failure-signal-rule)'s rule: the program chose an encoding that cannot represent its own text.)
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

**A recorded difference from `case`.** [§11.3](#sec-catch-is-case-for-exceptions) says that `catch` arms mirror `case` arms, and in a `case` a type pattern's binding has the pattern's written type (spec §19.3), so `case e of var f: IFailure` binds an `IFailure`. A `catch` arm over the same pattern binds an `Exception`. The two already differed for the multi-type alternative, whose binding is the nearest common base rather than any written type, and for the same reason: a `case` scrutinee may be anything, a caught value is known to be an `Exception`, and a handler's work is done on the exception. Keeping the mirror exact was weighed; it leaves the marker arm unable to log or wrap, which is what the arm is for.

**Why the marker has no members, and who may carry it.** With the binding an `Exception`, a member on the marker would be a second way to reach what the binding already has. Conformance is restricted to `Exception`-derived classes on the ground §11.7 used for arm types: a record or a non-exception class that lists the marker is a shape no program can observe, and Bezel rejects dead shapes rather than carrying them. A second restriction keeps the documents' own classification true: a class under a built-in or standard-library class that is not a failure may not list the marker. Without it `class C: OperationCanceledException, IFailure` would make a failure arm take a cancellation, and a user's failure under `InvalidOperationException` would be taken by every state-defect arm — the swallowing stdlib §4.8 keeps `KeyNotFoundException` clear of. The library's two bridging classes are the only ones across that line, each recorded with its reason; a user hierarchy under a user base stays free to mix kinds, since no arm on a class of the documents is affected.

**Unmarked means defect.** A user exception class is a defect report until it lists the marker. The other default would let a boundary arm swallow whatever a library author forgot to classify, which is the failure this entry exists to remove; the cost of the chosen default is one interface name on each class meant to be handled.

**What the marker does not do.** It forces nothing: no `throws` clause, no obligation to handle (spec §31.3). It changes no ownership rule, and bugs still unwind and clean up (spec K3.15). Abandoning the process or the domain on a defect was weighed and declined: it would skip the cleanup the exception edge guarantees, and it would make a contract violation uncatchable, where spec §22.16 makes it a catchable exception and the test oracle and the supervisor both depend on catching one. It leaves the query/assert tiers alone: a missing key and an empty collection stay defects in their throwing forms, because each has a query beside it.

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

**G2-1** — the opening list: replace

````text
- A **caller bug** is a contract (`requires`, §7) or defect exception.
- A **boundary fault** — I/O, network, anything logged or retried rather than branched on — is a typed exception (§4).
````

with

````text
- A **caller bug** is a contract (`requires`, §7) or a defect exception: a report, not something to handle.
- A **failure** — I/O, network, bad input: anything a correct program can meet, logged or retried rather than branched on — is a typed exception whose class implements `IFailure` (§4).
````

**G2-2** — §4, the opening sentence: replace

````text
For everything the value shapes don't cover — the boundary fault, and the defect a `requires` contract (§7) cannot state — the exception machinery is the familiar one.
````

with

````text
For everything the value shapes don't cover — the failure consumed at a boundary, and the defect a `requires` contract (§7) cannot state — the exception machinery is the familiar one.
````

**G2-3** — §4, new paragraphs after the hierarchy paragraph: insert after

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

The `IFailure` arm takes every failure and lets every bug through; `f` is an ordinary `Exception` inside the arm. The second arm is there only because this boundary is a supervisor that must keep serving. Most boundaries have no second arm: a bug then propagates, and an uncaught one ends the program with its report (spec §33.3). Where cancellation can reach a supervisor, give it its own arm ahead of the `Exception` arm — `OperationCanceledException => throw` — so that it is not reported as a bug. One arm on bare `Exception` that handles both is the habit to drop: it answers a bug as if it were bad input. An exception class of your own is a defect until you say otherwise:

```bezel
public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;
```

Two habits keep the kinds honest. Wrap a failure in a failure and never wrap a defect: re-raise it. And when out-of-range text matters, catch `FormatException` and `ParseRangeException` (or just `IFailure`); `ParseRangeException` is the failure `int.Parse("99999999999")` raises, distinct from the arithmetic `OverflowException` of `a * b`, which is a defect.
````

**G2-4** — §5, the `Handle` example: replace

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

**G2-5** — §7, the boundary row of the table: replace

````text
| a typed exception, producer-local (spec §31.4); read it in the `catch` and copy out what you keep (§5) |
````

with

````text
| a typed exception whose class implements `IFailure`, producer-local (spec §31.4); catch it at the boundary with an `IFailure` arm and copy out what you keep (§4, §5) |
| Handling whatever goes wrong at a boundary | an `IFailure` arm; a separate `Exception` arm only in a supervisor that reports the bug, never one arm for both (§4) |
````

**G2-6** — §7, the paragraph after the table: insert after

````text
Branching program logic on caught exception types at an interior call site is the signal to check the rule for a value-shaped tier.
````

the text

````text
 And at the boundary the arm names `IFailure`: an arm on bare `Exception` takes the bugs too.
````


#### Guide 4 (`docs/Bezel_Guide_04_Concurrency.md`)

**G4-1** — §4, the `Parallel` paragraph: replace

````text
First fault cancels remaining work and surfaces at the await as `DomainFaultedException` (stdlib §6.12).
````

with

````text
First fault cancels remaining work and surfaces at the await as `DomainFaultedException` — as its subclass `DomainFailedException` when that fault was a failure, so that an `IFailure` arm takes it (stdlib §6.12).
````

**G4-2** — §5, the paragraph on joins: insert after

````text
A fault in the entry surfaces at the join as `DomainFaultedException` carrying the original message and stack-trace *text* — exception objects themselves do not cross (spec §30.14).
````

the text

````text
 Its kind does cross: a failure in the child is joined as `DomainFailedException`, a `DomainFaultedException` that an `IFailure` arm also takes, and a bug in the child is joined as the plain class, which that arm lets pass (Guide 2 §4).
````


#### Guide 6 (`docs/Bezel_Guide_06_Contracts_and_Purity.md`)

**G6-1** — §2, the paragraph before the `Listen` example: replace

````text
the sanctioned signal is a typed exception (Guide 2 §4; the failure-signal rule's band 4, stdlib §1).
````

with

````text
the sanctioned signal is a typed exception whose class implements `IFailure`: a failure (Guide 2 §4; the failure-signal rule's band 4, stdlib §1).
````

**G6-2** — §2, the comment in the `Listen` example: replace

````text
    // Boundary-consumed expected failure: a typed exception, still never
    // a requires. FormatException is the built-in format fault (spec §31.4).
````

with

````text
    // Boundary-consumed expected failure: a typed exception, still never
    // a requires. FormatException is the built-in input failure: its class
    // implements IFailure (spec §31.4).
````

**G6-3** — §2, the paragraph after the `Listen` example: replace

````text
Why not just call the throwing `int.Parse(rawPort)` and let its exception fly, if you were going to throw anyway? Two reasons. The throwing form is the *assert-shaped* tier — stdlib §1's words are that it "treats absence as a bug" — so calling it on boundary input re-asserts, in exception spelling, the same misclassification the bad `requires` made. And it splits the condition: `int.Parse` raises `FormatException` for a malformed numeral but `OverflowException` for an out-of-range one (stdlib §9.1, §2.3), while `Listen`'s condition is single — *not a port*. The explicit throw keeps one condition on one signal, in the domain's own words. The same reasoning rules out `ArgumentException`:
````

with

````text
Why not just call the throwing `int.Parse(rawPort)` and let its exception fly, if you were going to throw anyway? You may. What it raises is a failure either way — `FormatException` for a malformed numeral, `ParseRangeException` for an out-of-range one (stdlib §9.1, §2.3) — because the text came from outside the program, and a boundary's `IFailure` arm takes both (Guide 2 §4). The explicit throw is for the wording: `int.Parse` reports two conditions in the numeral's terms, while `Listen`'s condition is single — *not a port*. The explicit throw keeps one condition on one signal, in the domain's own words. The kind is also what rules out `ArgumentException`:
````

**G6-4** — §2, the two-question paragraph: insert after

````text
a typed exception, as in the `Listen` rewrite (Guide 2 §4).
````

the text

````text
 The kinds follow the answer: a broken `requires` raises `ContractViolationException`, a defect report that a boundary's `IFailure` arm lets pass, and the typed exception for bad input is a failure, which that arm takes (spec §31.4).
````


#### Tour (`docs/Bezel_Tour.md`)

**T1** — §6 *Errors and nullability*, the exceptions paragraph: replace

````text
**Exceptions** are for the unexpected. A `catch` has comma-separated arms, matched in order:
````

with

````text
**Exceptions** are for the unexpected, and are of two kinds: a *failure* (a missing file, bad input), whose class implements `IFailure`, and a *defect* report, which is a bug. `IFailure => …` is the arm that takes every failure and no bug. A `catch` has comma-separated arms, matched in order:
````


#### Code-generation reference (`docs/Bezel_For_Code_Generation.md`)

**C1** — §5.1, *Three access tiers over one operation*: replace

````text
the **throwing** tier (absence is a bug) — `Parse`, `dict[key]`, `First()`, `PopAt`, `Pop`, `Dequeue`, `Console.ReadKey`;
````

with

````text
the **throwing** tier (the call asserts the value is there and raises otherwise: a *failure* where the condition is the outside's — `Parse`, `Console.ReadKey` — and a *defect* where it is the program's own state — `dict[key]`, `First()`, `PopAt`, `Pop`, `Dequeue`);
````

**C2** — §5.2, the `Convert` listing: replace

````text
end                                                    // faults: FormatException, OverflowException
````

with

````text
end                                                    // failures: FormatException, ParseRangeException
````

**C3** — §5.2, the exception hierarchy line: insert after

````text
`FormatException`, `StackOverflowException` (uncatchable).
````

the text

````text
 **Kinds (§31.4):** a class that implements the member-less interface `IFailure` is a *failure* — `FormatException`, the `IOException` family, `SerializationException`, `TimeoutException`, `ChannelClosedException`, `ChannelFullException`, `ParseRangeException` (an `OverflowException`: out-of-range `Parse`/`Convert.To*` input) and `DomainFailedException` (a `DomainFaultedException`: a failure joined from another domain); every other is a *defect* report; `OperationCanceledException` is neither. Only an `Exception`-derived class may list `IFailure`, and not one under a built-in or stdlib class that is not itself a failure.
````

**C4** — §5.6, the `Domain` listing: replace

````text
end                                   // the entry MUST be capture-free; EXACTLY ONE argument
````

with

````text
end                                   // the entry MUST be capture-free; EXACTLY ONE argument
                                      // a fault in the entry is joined as DomainFaultedException; as
                                      //   DomainFailedException (a subclass, IFailure) when it was a failure
````

**C5** — §5.9, the integer `Parse` line: replace

````text
out of range `OverflowException`;
````

with

````text
out of range `ParseRangeException` (an `OverflowException`; a failure);
````

**C6** — §5.9, the `Double`/`Float` `Parse` line: replace

````text
raises `OverflowException` exactly where the same digits as a literal of that type are out of range
````

with

````text
raises `ParseRangeException` exactly where the same digits as a literal of that type are out of range
````

**C7** — §6, *Declaring, throwing and catching an exception type*: replace

````text
Derive from `Exception` (or any built-in exception type) and chain the message to the base; a caught exception is a **borrow** valid inside its arm only, so copy out what you keep.

```bezel
public class ConfigError(message: string): Exception(message)
end ConfigError;
````

with

````text
Derive from `Exception` (or any built-in exception type) and chain the message to the base; list `IFailure` when the condition comes from outside the program, since an unmarked class is a defect report (§31.4); a caught exception is a **borrow** valid inside its arm only, so copy out what you keep.

```bezel
public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;
````

**C8** — §6, a new idiom after the exception-wrapping paragraph: insert after

````text
never retained (§31.4); there is no user `InnerException`.
````

the text

````text
**Catching at a boundary.** Name the marker, not `Exception`: an `IFailure` arm takes every failure and lets every bug through, and its binding is an ordinary `Exception` (§31.2.1).

```bezel
try
  Serve(request);
catch
  var f: IFailure => Respond(502, f.Message),                  // failures: answered
  var ex: Exception => do ReportDefect(ex); Respond(500); end  // a supervisor only: bugs reported
end;
```

Leave the second arm out unless the code is a supervisor: a bug should propagate. `IFailure => ()` suppresses failures only. To reach an interface's own members inside an arm, test in the body: `var r := ex as IRetryable;`.
````

## 11. The condition table

Every condition for which a standard-library section says a member raises a named exception type, by section (116 rows; generated from `data/taxonomy.tsv` by `data/taxonomy_check.py --markdown`). *Origin* is where the condition comes from (§4.1): outside the program — **environment**, **data** being read in, **peer**, **limit** — or the program itself — **argument**, **state**, **arithmetic**; **request** is a cancellation the program asked for and **carried** is a domain join. *Kind* follows from the origin. *Action*: **none** — the type's kind already matches; **marker** — the type implements `IFailure`, directly or by inheritance; **range-class** — the member raises ▲`ParseRangeException`; **join-class** — the join raises ▲`DomainFailedException` when the original was a failure; **misfit** — a defect condition raised with a failure class, flagged and not moved; **relabel** — the section's band changes, the type does not.

| Stdlib § | Condition | Type today | Origin | Kind | Action |
|---|---|---|---|---|---|
| 1 | a negative `capacity`, `size` or `length` constructor argument (#1396) | `ArgumentOutOfRangeException` | argument | defect | none |
| 1 | `Channel<T>(capacity)` with `capacity < 1` (restated at 6.9) | `ArgumentException` | argument | defect | none |
| 2.1 | `Console.ReadKey()` at end of input | `EndOfStreamException` | environment | failure | marker |
| 2.2 | `Math.Clamp` with `low > high` | `ArgumentException` | argument | defect | none |
| 2.2 | `Math.DivRem(a, 0)` | `DivideByZeroException` | arithmetic | defect | none |
| 2.3 | `Convert.ToString(value, toBase)` with a base outside 2, 8, 10, 16 | `ArgumentException` | argument | defect | none |
| 2.3 | `Convert.To*`: the text is not a well-formed value | `FormatException` | data | failure | marker |
| 2.3 | `Convert.To*`: the value does not fit the target type | `OverflowException` | data | failure | range-class |
| 2.4 | `Environment.Set` with an empty name or one containing `=` | `ArgumentException` | argument | defect | none |
| 2.5 | a `DateTime`/`DateOnly`/`TimeOnly` constructor component outside its range | `ArgumentOutOfRangeException` | argument | defect | none |
| 2.5 | `TimeSpan` `*`, `/`, unary `-` and the `From*` factories: a result outside the tick range, or a `NaN` operand | `OverflowException` | arithmetic | defect | none |
| 2.5 | `TimeSpan / 0.0` | `DivideByZeroException` | arithmetic | defect | none |
| 2.5 | `DateTime.Parse`, `DateOnly.Parse`, `TimeOnly.Parse`: any other input | `FormatException` | data | failure | marker |
| 2.5 | `DateTime.Add*`, `Add`, `Subtract(span)`, `FromTicks`: a result outside `MinValue..MaxValue` | `OverflowException` | arithmetic | defect | relabel |
| 2.6 | `Guid.FromByteArray` with `Count <> 16` | `ArgumentException` | argument | defect | none |
| 2.6 | `Guid.Parse`: any other input | `FormatException` | data | failure | marker |
| 2.6 | `Guid.ToString(format)`: an unrecognized specifier | `FormatException` | argument | defect | misfit |
| 2.7 | `Random.Next` with `maxValue < 0` or `minValue > maxValue` | `ArgumentOutOfRangeException` | argument | defect | none |
| 2.13 | `Type.EnumNames` / `EnumValues` on a non-enum `Type` | `InvalidOperationException` | state | defect | none |
| 2.14 | synthesized `Deserialize`: an unregistered discriminator, or malformed input | `SerializationException` | data | failure | marker |
| 2.14 | synthesized `Serialize`: an unregistered run-time type, or a `Weak` reference to a target never strongly reached | `SerializationException` | state | defect | misfit |
| 2.15 | read side: malformed input, a type mismatch, an out-of-range number, a depth-limit violation, an unknown field under the strict policy, trailing data | `SerializationException` | data | failure | marker |
| 2.15 | JSON write side: a non-finite float (the section already flags it) | `SerializationException` | state | defect | misfit |
| 2.18 | `ToString(format)` and interpolation specs: an unrecognized specifier | `FormatException` | argument | defect | misfit |
| 2.19 | `IParsable<T>.Parse`: bad input | `FormatException` | data | failure | marker |
| 2.19 | `ISteppable<T>.Step`: a result not representable in `T` | `OverflowException` | arithmetic | defect | none |
| 3.1 | `StringBuilder.AppendFormat`: a malformed format string, a hole index outside `args`, a spec on a type that accepts none | `FormatException` | argument | defect | misfit |
| 3.1 | `StringBuilder.Insert` / `Remove`: an index or window outside `0..Length` | `IndexOutOfRangeException` | argument | defect | none |
| 3.1 | `StringBuilder.Replace` with an empty `oldValue` | `ArgumentException` | argument | defect | none |
| 3.2 | `Encoding.GetString`: bytes that are not a well-formed sequence in the encoding | `FormatException` | data | failure | marker |
| 3.2 | `Encoding.GetBytes`: a code point the encoding cannot represent (`ASCII` only) | `FormatException` | argument | defect | misfit |
| 3.2 | `GetBytes` into a destination span that is too short | `IndexOutOfRangeException` | argument | defect | none |
| 3.3 | `ReadOnlyStringSpan` indexer and slices out of range | `IndexOutOfRangeException` | argument | defect | none |
| 3.3 | `ReadOnlyStringSpan.CopyTo` with a length mismatch | `ArgumentException` | argument | defect | none |
| 4.1 | a mutating member of a user-defined read-only collection | `InvalidOperationException` | state | defect | none |
| 4.3 | `Pop`, `PopFirst`, `PopLast`, `Dequeue`, `Peek` on an empty collection (query duals: the `Try*` forms) | `InvalidOperationException` | state | defect | none |
| 4.3 | `PopAt(index)` outside `0..<Count` | `IndexOutOfRangeException` | argument | defect | none |
| 4.4 | `Array<T>` constructors with a negative capacity or size | `ArgumentOutOfRangeException` | argument | defect | none |
| 4.4 | the indexer and the slice windows out of range | `IndexOutOfRangeException` | argument | defect | none |
| 4.4 | a structural mutation from inside the collection's own sort comparer | `InvalidOperationException` | state | defect | none |
| 4.5 | `OwningArray<T>(capacity)` with a negative capacity | `ArgumentOutOfRangeException` | argument | defect | none |
| 4.5 | the indexer out of range | `IndexOutOfRangeException` | argument | defect | none |
| 4.8 | `dict[key]` on an absent key, compound assignment included (query duals: `GetOrNull`, `TryGetValue`) | `KeyNotFoundException` | state | defect | none |
| 4.8 | `Add(key, value)` / `Add(entry)` on a present key (query dual: `TryAdd`) | `ArgumentException` | state | defect | none |
| 4.8 | a structural mutation from inside `GetOrAdd`'s factory; the note on `KeyNotFoundException`'s base | `InvalidOperationException` | state | defect | none |
| 4.9 | `OwningDictionary.Add` on a present key | `ArgumentException` | state | defect | none |
| 4.9 | the indexer on an absent key | `KeyNotFoundException` | state | defect | none |
| 4.13 | `TakeCurrent()` out of protocol | `InvalidOperationException` | state | defect | none |
| 4.14 | indexers, slices, the `Read*`/`Write*` accessors and bit views out of range (query duals: `TryRead*`, `TryWrite*`) | `IndexOutOfRangeException` | argument | defect | none |
| 4.14 | `MoveFrom` / `CopyFrom` with a length mismatch | `ArgumentException` | argument | defect | none |
| 4.14 | a bit `count` outside 1..64 | `ArgumentOutOfRangeException` | argument | defect | none |
| 4.14 | `AlignedTail()` on a view that is not byte-aligned | `InvalidOperationException` | state | defect | none |
| 4.14 | `span[i]!` on a null slot | `NullReferenceException` | state | defect | none |
| 4.14 | a read through a span whose slot value was disposed (checked disposal mode) | `DisposedAccessException` | state | defect | none |
| 4.15 | indexers and slices out of range | `IndexOutOfRangeException` | argument | defect | none |
| 4.15 | `CopyFrom` with a length mismatch | `ArgumentException` | argument | defect | none |
| 4.15 | a bit `count` outside 1..64 | `ArgumentOutOfRangeException` | argument | defect | none |
| 4.15 | `AlignedTail()` on a view that is not byte-aligned | `InvalidOperationException` | state | defect | none |
| 4.17 | the indexer on an absent key | `KeyNotFoundException` | state | defect | none |
| 4.17 | `Add` on a present key | `ArgumentException` | state | defect | none |
| 4.17 | `FirstKey` / `LastKey` on an empty dictionary; a structural modification during iteration | `InvalidOperationException` | state | defect | none |
| 4.18 | `First` / `Last` on an empty set | `InvalidOperationException` | state | defect | none |
| 4.19 | the `FrozenArray<T>` indexer out of range | `IndexOutOfRangeException` | argument | defect | none |
| 4.21 | `OwningSlotMap<T>(capacity)` with a negative capacity | `ArgumentOutOfRangeException` | argument | defect | none |
| 4.21 | `Get` / `PopValue` on an absent handle (query duals: `GetOrNull`, `TryGetValue`, `TryPopValue`) | `KeyNotFoundException` | state | defect | none |
| 4.21 | a handle minted by another map | `ArgumentException` | argument | defect | none |
| 4.21 | a mutation through a live view | `InvalidOperationException` | state | defect | none |
| 5 | a second enumeration of a single-consumption query; `First`/`Last`/`Single` (and predicate forms), `Min`/`Max`/`Average` with no element (query duals: the `*OrNull` forms) | `InvalidOperationException` | state | defect | none |
| 5 | `ElementAt` out of range (query dual: `ElementAtOrNull`) | `IndexOutOfRangeException` | argument | defect | none |
| 5 | `ToDictionary` on a duplicate key | `ArgumentException` | state | defect | none |
| 5 | `Sum` past the element type's range | `OverflowException` | arithmetic | defect | none |
| 5.1 | `Min()` / `Max()` / `Average()` on an empty source | `InvalidOperationException` | state | defect | none |
| 6.1 | `Register` on a published source; publishing a source that holds registrations | `InvalidOperationException` | state | defect | none |
| 6.1 | `ThrowIfCancellationRequested()` | `OperationCanceledException` | request | cancellation | none |
| 6.2 | a blocking acquire where a domain continuation would park | `InvalidOperationException` | state | defect | none |
| 6.2 | a cancelled `AcquireAsync` / `TryAcquireAsync` | `OperationCanceledException` | request | cancellation | none |
| 6.3 | a blocking acquire where a domain continuation would park | `InvalidOperationException` | state | defect | none |
| 6.3 | a cancelled async acquisition | `OperationCanceledException` | request | cancellation | none |
| 6.3 | `new Semaphore(initial, max)` with `max < 1` or `initial > max` | `ArgumentException` | argument | defect | none |
| 6.4 | an `AtomicInteger<T>` operation whose result is not representable | `OverflowException` | arithmetic | defect | none |
| 6.5 | a cancelled handler task under `RaiseAsync` | `OperationCanceledException` | request | cancellation | none |
| 6.7 | `WhenAny` over an empty array | `ArgumentException` | argument | defect | none |
| 6.7 | a second `GetAsyncEnumerator` on a `WhenEach` sequence; the `FromException` example | `InvalidOperationException` | state | defect | none |
| 6.7 | `FromCancelled`, and a cancelled input at `WhenAll` / `WhenEach` | `OperationCanceledException` | request | cancellation | none |
| 6.8 | the join of an entry that raised | `DomainFaultedException` | carried | carrier | join-class |
| 6.9 | `Channel<T>(capacity)` with `capacity < 1` | `ArgumentException` | argument | defect | none |
| 6.9 | the synchronous `Send` on a full bounded channel | `ChannelFullException` | limit | failure | marker |
| 6.9 | `Receive` after the drain of a closed channel; `Send`/`SendAsync` after close; a `SendAsync` pending at close | `ChannelClosedException` | peer | failure | marker |
| 6.9 | a second concurrent consumption of the single-consumer end | `InvalidOperationException` | state | defect | none |
| 6.10 | `TaskScope`: `DisposeAsync` without `Join`, `Spawn` after close | `InvalidOperationException` | state | defect | none |
| 6.11 | a blocking form where a domain continuation would park | `InvalidOperationException` | state | defect | none |
| 6.11 | a cancelled pending acquisition | `OperationCanceledException` | request | cancellation | none |
| 6.12 | `Parallel.*` with `degree < 0` | `ArgumentException` | argument | defect | none |
| 6.12 | the first body fault, at the `await` | `DomainFaultedException` | carried | carrier | join-class |
| 7.1 | a `Read`/`Write` window outside the buffer's `Count` | `IndexOutOfRangeException` | argument | defect | none |
| 7.2 | `FileStream` construction: the file does not exist under `FileMode.Open` | `FileNotFoundException` | environment | failure | marker |
| 7.2 | `FileStream` construction: a directory component does not exist | `DirectoryNotFoundException` | environment | failure | marker |
| 7.2 | `FileStream` construction: the OS reports a too-long path | `PathTooLongException` | environment | failure | marker |
| 7.2 | `FileStream` construction: every other OS-level open failure | `IOException` | environment | failure | marker |
| 7.3 | the `File` text members: ill-formed UTF-8 on read | `FormatException` | data | failure | marker |
| 7.4 | `Directory.Delete` of a non-empty directory without `recursive`; the other `Directory`/`Path` failures | `IOException` | environment | failure | marker |
| 7.4 | the other `Directory`/`Path` failures | `FileNotFoundException` | environment | failure | marker |
| 9.1 | integer `Parse` (string, radix and span forms): input outside the grammar | `FormatException` | data | failure | marker |
| 9.1 | integer `Parse`: a value outside the type's range | `OverflowException` | data | failure | range-class |
| 9.1 | `Parse` / `TryParse` / `ToString` with a radix outside 2, 8, 10, 16 | `ArgumentException` | argument | defect | none |
| 9.2 | `double.Parse` / `float.Parse`: input outside the grammar | `FormatException` | data | failure | marker |
| 9.2 | `double.Parse` / `float.Parse`: a value that is not finite once rounded | `OverflowException` | data | failure | range-class |
| 9.3 | `decimal.Parse`: input outside the grammar | `FormatException` | data | failure | marker |
| 9.3 | `decimal.Parse`: a value outside `decimal`'s range | `OverflowException` | data | failure | range-class |
| 9.3 | `Round(digits)` with `digits` outside 0..28 | `ArgumentOutOfRangeException` | argument | defect | none |
| 9.5 | `char.Parse`: any input but one code point | `FormatException` | data | failure | marker |
| 9.6 | `Repeat` with `count < 0`; `PadLeft`/`PadRight` with `totalWidth < 0` | `ArgumentOutOfRangeException` | argument | defect | none |
| 9.6 | `IndexOf` start, and the window members, out of range | `IndexOutOfRangeException` | argument | defect | none |
| 9.8 | `GetEnumerator` on an open-ended range | `InvalidOperationException` | state | defect | none |
| 10.3 | a copy window outside the destination's `Count` | `IndexOutOfRangeException` | argument | defect | none |
| 10.4 | `string.FromCStringPointer(IntPtr.Zero)` / `FromWStringPointer(IntPtr.Zero)` | `ArgumentNullException` | argument | defect | none |

The language's own raise sites are §31.4's tree and need no table: every built-in operation that raises (an indexer, arithmetic, a checked conversion, `!`, a non-exhaustive `case` expression, a contract clause, static initialization) raises a defect; `await` on a cancelled task raises cancellation.

## 12. The catch-all warning: designed, corrected, and not proposed

This section is a record for a later proposal. Nothing in §10 depends on it.

**The rule, as corrected after the critique.** An unguarded arm whose pattern names `Exception` alone — bare or binding, not in an alternative — would draw a warning unless (a) an earlier unguarded arm of the same `catch` block lists `IFailure`, alone or in an alternative, or (b) its body contains a bare re-raise.

- **`Exception => ()` is not exempt.** Revision 1 exempted the unit body, which exempted the widest swallow while warning on an arm that logs. Under the corrected rule §31.2.5's suppression idiom warns unless it follows an `IFailure` arm, and the two spellings that say what is meant are `IFailure => ()` (failures only) and `IFailure => (), Exception => ()` (everything, with the division written out).
- **What satisfies (a).** An earlier unguarded arm that lists `IFailure` itself. An interface that extends `IFailure` does not: it takes only some failures. A guarded `IFailure` arm does not: its guard may decline.
- **Both kinds in one body, on purpose:** `var ex: IFailure | Exception => …`, which is not an arm naming `Exception` alone (compiles and runs on r40, f19 case 3).
- **Known bypasses.** Any guard on the `Exception` arm (`when true` included) takes it out of the rule. A conversion (`Exception => throw new Wrapped(…)`) is *not* a bypass: it has no bare re-raise, and a wrapper may be a failure class.
- **Locality.** One `catch` block's arm patterns and one arm body; no raise and no arm selection is modelled, so the decline of type-keyed reachability (proposal 0198 §7) is not engaged.

**Reach** (MEASURED, `catch_arms_summary.json`): 572 arms in the four corpora — 545 that handle, 21 that convert, 6 that suppress. None is in a rule case with `expect: quiet`; 12 lit files contain one and also assert on the absence of diagnostics (`data/undivided_lit_files.txt`). The document-example corpus has no expectation that asserts silence (its classes are `compiles`, `diagnose` and `skip`), so no extracted example would fail. The 40 everyday programs draw it 0 times.

**Document sites it would touch** (MEASURED, the *catchall* group of `data/doc_sites.tsv`): spec §31.1's closure example and §31.2.5's `Exception => ()`; stdlib §6.5's handler example; rationale §13.5's per-handler swallow; and the two examples §10 already rewrites (S6, G2-4). Two more are compile-error examples and are unaffected.

**Why it is not proposed now, and what would bring it back.** §5.4 gives the four reasons. The trigger is evidence of the habit outside the test corpora after the marker exists: programs written against a compiler that has `IFailure` — the ergonomics study rerun, or code-generation transcripts — in which a boundary handles everything in one `Exception` arm. It would land warn-only; a promotion to an error would be a further proposal.

## 13. Probes and data

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
| f15 | a record and a plain class listing the marker | both compile today (the first shape §3.3 rejects) |
| f16 | `throw f` with a marker-typed operand | rejected (§31.1) |
| f17 | a `sealed` marked exception class as a spawn argument | rejected: not sendable (§30.14) |
| f18 | an interface-typed value passed to a class-typed parameter | **accepted**, against §5.1; with a non-exception object the callee reads an empty `Message` (a compiler gap; the proposal does not rely on it) |
| f19 | the guide's idioms: a failure class with a primary constructor; a supervisor boundary; `IFailure \| Exception`; `IFailure => ()` | runs |
| f20 | the straddling types in r40's own library | `Parse` out of range and an arithmetic overflow reach one `OverflowException` arm; malformed input and a bad format specifier reach one `FormatException` arm |
| f21 | an interface's own members inside an arm: `var r := ex as IRetryable`; and the guard form | runs: `as` keeps `ex.Message` in reach; the guard form reaches the interface member |
| f22 | `var ex: Exception when ex is IRetryable => ex.Message` | **rejected**: the guard retypes `ex` to the interface (§12.7) |
| f23 | a class under `OperationCanceledException`, and one under `InvalidOperationException`, each listing the marker | both compile today: a marker arm takes the cancellation, a state-defect arm takes the "failure" (the second shape §3.3 rejects) |

Data (`data/`):

| File | Content |
|---|---|
| `catch_arms.py` → `catch_arms_summary.json`, `undivided_lit_files.txt` | every catch arm in the four corpora, found by walking each `catch` block, with its body classified; the catch-alls of §12; the 40 everyday programs |
| `band_reach.py` → `band_reach.json`, `overflow_over_parse_files.txt` | the reach numbers of §2, §3.2 and §9.1 |
| `taxonomy.tsv`, `taxonomy_check.py` | the condition table with each row's origin; its completeness check against the stdlib document; the check that kind follows origin |
| `edits.py` | the edit set of §10: `check`, `apply`, `render`, `twin` |
| `doc_sites.py` → `doc_sites.tsv` | the audit of §9.2: every affected line of the documents, edited or left with a reason |

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
python3 $D/doc_sites.py .
docs/research/error-model/probes-0211/run_all.sh
```

**Compiler gaps met, informational, not filed.** (1) f18: an interface-typed argument is accepted at a class-typed parameter. (2) f03: a property inherited from `Exception` does not satisfy an interface property. (3) f05: no unreachable-arm warning where an interface arm covers a later class arm. (4) `ToString()` on the binding of a multi-type alternative is rejected (the diagnostic cites #2457). (5) A native callback segfaults, including rule case `s32_8_0021`'s program. The note's list (`SupersededChain` absent, `Exception.ToString()` without the message, `FaultStackTrace` unimplemented) still holds on r40.

## 14. Limits and self-critique

- **Revision 2 has not been re-critiqued.** Revision 1 was rejected as written; this revision answers each finding and has had no independent pass. [`spec-change-process.md`](spec-change-process.md) §4 requires one before a landing.
- **The origin test has judgement calls.** `ChannelFullException` could be read as the program's choice of the wrong `Send`; `ToDictionary` on a duplicate key and `Guid.FromByteArray` on a wrong length depend on data and are ruled the program's; arithmetic on external data is argued in §4.1 and is the weakest ruling; `TypeInitializationException` hides a failure behind a defect. Each has a row and an origin in §11, and Q7 and Q8 put them to the owner.
- **The gloss rewrite changes what Guide 6 teaches.** After it, `int.Parse(untrusted)` handled at a boundary is correct code. That is a consequence of one class having one kind, not a goal, and Q6 asks for it to be ruled on its own.
- **The second conformance restriction is new in this revision** and has no probe beyond f23, which shows the shape it rejects. It forbids a user class under `ArgumentException` or `InvalidOperationException` from being a failure, which some existing habits would want; Q5 gives the narrower alternative.
- **The table covers what the standard library names.** A condition a section leaves unstated is not in it. Exception types declared by proposals that have not landed are not classified here: a grep of proposals 0001–0139 finds 24 such classes, 14 of which derive from the `IOException` family or from `FormatException` and would be failures by inheritance; the other ten (`JsonException` and its three subclasses, `HttpResponseException`, `CryptographicException`, the two time-zone classes, `AssertionException`, `VerificationException`) state their kind when their proposal graduates (MEASURED, by grep).
- **The audit of §9.2 finds lines by pattern.** A sentence that states an affected rule in other words is not found by it. The six patterns were chosen from the critique's list and from reading; they are in `doc_sites.py` to be extended.
- **A bug can sit behind a failure in two places, both as today.** `Task.WhenAll` re-raises the first non-successful input in input order (§6.2), and a cleanup failure supersedes a defect (§6.6). The marker makes both more visible; neither is changed.
- **The proposed binding form does not compile on r40**, which binds a single interface arm at the interface (f04). The idioms were run with the forms r40 accepts (f19); the alternative form shows the proposed typing (f06).
- **FFI could not be probed** (§6.5). §32.8's text is cited instead.
- **Prior art is from memory.** The Java observation in §3.3 and in the proposed rationale text, and the remark on Rust, Swift and Midori in §4.1, should be checked against a source before landing.
- **Conformance level.** §31 is Core and §30.14 is in the Asynchronous-programming feature (Appendix D), so the marker and the arm rule are Core and ▲`DomainFailedException` is Extended. No Appendix D text changes.
- **Proposal 0110** (under review) touches `DomainFaultedException`; §6.3 states how the two compose, and whichever lands second re-reads the other.

## 15. Owner questions

**Q1 — The word for the bug band.** Normative text says *defect* and *failure*, and *fault* keeps meaning "an exception was raised" (330 uses in the spec and stdlib). The alternative is to adopt *fault* for the bug band and sweep those uses. **Recommendation: *defect*.**

**Q2 — The marker's shape.** An interface ▲`IFailure` with no members, or a base class `Failure: Exception`. The interface keeps every existing arm matching and makes a later re-banding an added interface; the base class needs no catch-arm rule and flips 26 `OverflowException` arms and the failure-carrying `DomainFaultedException` arms. **Recommendation: the interface**, and the name `IFailure`.

**Q3 — The catch-binding rule.** A binding arm over an interface binds an `Exception` (as r40 already does for an alternative), at the cost of one rule-corpus program and of reaching an interface's own members through `as` inside the arm. The alternative keeps §31.2.1 as it is and declares `Message`, `StackTrace` and the chains on the marker; the binding then still cannot be passed or chained. **Recommendation: the binding rule.**

**Q4 — The difference from `case`.** Under Q3 a `catch` arm `var f: IFailure` binds an `Exception`, where `case e of var f: IFailure` binds an `IFailure` (§19.3), against rationale §11.3's statement that the two mirror each other. The multi-type catch binding already differs in the same way. **Recommendation: accept the divergence**, stated in §31.2.1 and recorded in the rationale. Rejecting it means the alternative under Q3.

**Q5 — Who may list the marker.** (a) Only `Exception`-derived classes. (b) Not a class whose base chain contains a class the documents declare, other than `Exception`, that is not a failure: so no failure under `OperationCanceledException`, and none under `InvalidOperationException`, `ArgumentException` or any other defect class; a user hierarchy under a user base stays free. The narrower alternative to (b) forbids only `OperationCanceledException` subclasses and lets user code mark a subclass of a defect class, which is how the two library bridges are built and which recreates the swallowing stdlib §4.8 guards against. **Recommendation: (a) and (b) as written**; (b) can be relaxed later without breaking a program, and the reverse cannot.

**Q6 — The kind is the condition's, not the tier's.** A condition is a failure or a defect by where it comes from, whichever tier raises it; the three glosses that call a throwing form "a bug at this call site" (stdlib §1 twice, stdlib §2.1) and Guide 6 §2's use of them are rewritten, so that the throwing `Parse` and `ReadKey` raise failures and the throwing indexer a defect. After this, `int.Parse(untrusted)` handled at a boundary is correct code. **Recommendation: yes.** The alternative keeps the glosses, and then `FormatException` cannot be both what `Parse` raises "as a bug" and the input failure Guide 6 throws; one of the two would need a new class.

**Q7 — Arithmetic on external data.** `Sum`, `TimeSpan` arithmetic and `DateTime.Add*` out of range are defects, like `a * b`, although whether they occur depends on input and the library offers no query for them. This reverses v0.292's band-4 label for the `DateTime` range fault (the type, and the decline of `ArgumentOutOfRangeException`, are kept). The alternative keeps that one condition in band 4 with a third bridging class under `OverflowException`, which splits it from `TimeSpan` arithmetic. **Recommendation: a defect, and the label reversed.**

**Q8 — The rest of the taxonomy.** Confirm: (a) a missing key, an empty collection and an index out of range are defects because the program owns the state tested; (b) `ChannelClosedException`, `ChannelFullException` and `TimeoutException` are failures; (c) `TypeInitializationException` is a defect whatever its inner exception; (d) `OperationCanceledException` is neither, in every subclass. **Recommendation: as tabled in §4 and §11.**

**Q9 — The two bridging classes.** ▲`ParseRangeException: OverflowException, IFailure` and ▲`DomainFailedException: DomainFaultedException, IFailure`, each a failure class under a defect class so that existing arms keep matching. The alternative is a clean cut: out-of-range input under `FormatException` (26 arms in 13 files flip) and a sibling join class (the arms on `DomainFaultedException` stop taking failures). **Recommendation: the bridges, with these names.**

**Q10 — The six misfits.** A bad format specifier (stdlib §2.18, §2.6, §3.1), the write-side serialization conditions (stdlib §2.14, §2.15) and the unrepresentable code point at `GetBytes` (stdlib §3.2) are defects raised with a failure class. A kind cannot be shed, so fixing one means a class outside the failure class's subtree and broken arms, now or later, and later costs more. The choice is therefore: re-type in this landing (for the specifier, at least 36 arms in 9 test files; the others unmeasured), or keep the misfits, in practice for good. **Recommendation: keep them, flagged.** Each fails on a first test run of its line, and each re-typing is a stdlib design question of its own (which defect class); but if the owner wants none in the library, this landing is the cheapest moment there will be.

**Q11 — The catch-all warning.** Defer it to its own proposal, with §12 as its starting design and evidence from programs written with the marker as its trigger; or adopt §12's rule in this landing, warn-only. **Recommendation: defer.** The evidence is test oracles and the documents' own examples, the 40 everyday programs have no catch-all, nothing else depends on it, and it is non-breaking to add later.

**Q12 — User classes.** A user exception class is a defect until it lists the marker. **Recommendation: yes.**

**Q13 — Three things left out on purpose.** (a) Re-raising a defect ahead of a failure at `Task.WhenAll` and `TaskScope`, which would change §30.10's first-in-input-order contract; (b) a kind field on `ExceptionInfo`; (c) any rule for a cleanup failure that supersedes a defect, beyond the guard idiom of §6.6. **Recommendation: leave all three out; say if any is wanted as its own proposal.**

**Q14 — One landing.** The marker, the taxonomy, the two classes and the binding rule in one revision, with the five trackers of §9.6; or the marker and taxonomy first and the domain join second, as the note's stage 1b had it. **Recommendation: one revision**, after a second adversarial critique of this text.
