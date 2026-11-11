# Proposal 0214 — The narrowed failure marker: origin on the class, disposition in the handler

| | |
|---|---|
| **Status** | graduated-v0.358 — superseded — accepted 2026-11-11 by owner decision ("Narrowed marker"; and, for §6, "Withdraw; OrNull is doctrine"), graduated v0.358 with the edits of §9 unchanged in their anchors; landed with the v0.358 changelog entry |
| **Tracking issue** | [#8407](https://github.com/s8lap/bezel-lang/issues/8407) (proposal 0211's, which this revises); trackers [#8414](https://github.com/s8lap/bezel-lang/issues/8414), [#8416](https://github.com/s8lap/bezel-lang/issues/8416), [#8417](https://github.com/s8lap/bezel-lang/issues/8417), [#8418](https://github.com/s8lap/bezel-lang/issues/8418) updated; [#8516](https://github.com/s8lap/bezel-lang/issues/8516) filed |
| **Target documents** | **spec** — §30.14 *Faults* (one sentence), §31.1 (two sentences), §31.2.4 (one comment), §31.2.5 (one sentence), §31.4 (*The failure marker*, *Who may conform*, the paragraph after it and the one on what reads the marker; the tree's `FormatException` line; the first and last paragraphs), Appendix C.7 (one bullet), Appendix E (one row). **stdlib** — §1 (the access tiers' gloss, bands 1 and 4, *Failures and defects* replaced by *The failure marker*, the no-dual rule's stance sentence), §2.3, §2.5, §2.6, §2.14, §2.15, §2.18, §3.1, §3.2, §6.8, §9.1 (*The range failure* removed), §9.2, §9.3. **rationale** — §11.4 (one sentence), §11.12 (heading and five paragraphs). **Guide 2** §2, §4, §5, §7; **Guide 4** §5, §8 (re-traced in full); **Guide 6** §2; the **Tour** §6; the **code-generation reference** §5.1, §5.2, §5.9, §6. **proposals** — `README.md`, `DECLINED.md`, proposal 0211's status row. No grammar change, no new keyword, no new diagnostic; one rejection narrowed. |
| **Written against** | spec, stdlib and rationale **v0.357** (tree `e49c32aca`); the research note [`docs/research/failure-kind-by-context/`](../research/failure-kind-by-context/README.md) at `1940edc7a` and its re-rating |
| **Origin** | The owner's objection to proposal 0211 (2026-11-10): whether a condition is expected depends on the use — a file the user chose against the database file a service cannot run without — so the library should not prescribe it. The research note measured the objection; the owner decided on 2026-11-11. |
| **Change class** | Design change ([`spec-change-process.md`](spec-change-process.md) §2): it reopens recorded rulings of proposal 0211 §16 and recorded `DECLINED.md` rows, each with the new evidence of §2 and §5 below. Nothing here is a ratchet. |

*Conventions. An unprefixed § is a language-spec section at v0.357; stdlib, rationale, guide and note sections are marked. "The note" is the research note above. Evidence labels are the note's: **MEASURED** (a script or a re-runnable count), **QUOTED** (repository text), **HYPOTHESIS** (a judgement). ▲ marks nothing: this proposal adds no name.*

## 0. Summary

Proposal 0211 (v0.356) gave every standard-library exception class a *kind*: a **failure** (implements `IFailure`; "a condition a correct program can meet") or a **defect** ("the program is wrong"). The note measured that against 47 tasks and 272 task–operation pairs, and an independent blind re-rating of 100 of them reproduced every headline. Three findings decide this proposal:

- **The environment and peer classes state origin correctly.** The I/O family, network, end of stream, time-out and the channel exceptions are a bug in 3 of 106 rows. What a program *does* about them still varies with the call — a missing file is handled at the call in 14 rows and fatal in 8 — so the class may state origin and nothing else.
- **The data classes do not.** `FormatException` and `SerializationException` are a bug in 8 of their 66 rows, and those are most of the bugs v0.356 raised with a failure class. Whether the text was a user's or the program's own constant decides, and the library cannot see it.
- **The defect side is refuted.** The classes v0.356 called defects are a bug in 37 of 89 rows: an unmarked class cannot say "the program is wrong".

So, by the owner's decision of 2026-11-11 ("Narrowed marker"):

1. **`IFailure` stays only on the environment and peer classes** — the `IOException` family, `TimeoutException`, `ChannelClosedException`, `ChannelFullException`, and `DomainFailedException`, which carries such a failure across a domain join. Its meaning is **origin and a default**: "the outside world disagreed". What to do about a failure is always the handler's decision.
2. **`FormatException` and `SerializationException` lose the marker**, and **`ParseRangeException` is retired**: out-of-range `Parse` input raises `OverflowException` again (§3.4). The six recorded misfits disappear with it.
3. **An unmarked class makes no statement.** The word *defect* leaves the normative text (§3.7).
4. **Who may conform is relaxed** to the cancellation exclusion alone: a user class may list `IFailure` under any base but `OperationCanceledException` (§3.6).
5. **The inconsistency v0.356 left** between spec §31.2.2's example and Guide 2 §4 (which handle `ArgumentException` and `InvalidOperationException` as expected input) and stdlib §1's "a defect surface is never a recovery surface" is removed, with the other over-claims (§4).
6. **Deferred:** the caller-side "this failure is a bug here" conversion (§7.1). **A follow-up, not written here:** checked arithmetic (§7.2).
7. **The call-site `(try E)` form is withdrawn as a direction** (owner, 2026-11-11, "Withdraw; OrNull is doctrine"): the `*OrNull`/`Try` tiers are the doctrine for value-shaped results, and a missing dual is a library gap, filed as [#8516](https://github.com/s8lap/bezel-lang/issues/8516) (§6).

Reach: no compiler names the marker (MEASURED: `git grep IFailure -- src tests` outside the rule corpus finds nothing), no program outside the rule corpus names it, 8 rule-corpus cases change verdict and 8 are added (§8).

## 1. The decision, as recorded

**Owner decision, 2026-11-11, "Narrowed marker"** (relayed by the coordinator; QUOTED from the brief):

> 1. `IFailure` stays only on the environment and peer classes — I/O (the IOException family incl. FileNotFound/DirectoryNotFound/permission as they exist in the stdlib), network, end-of-stream, timeout, channel (closed/full) — and its meaning is restated as **origin and a default**: "the outside world disagreed". What to do about it is always the handler's decision; catching a missing database file and stopping is just a handler.
> 2. Remove it from `FormatException`, `SerializationException` and `ParseRangeException` …
> 3. An unmarked class makes **no statement**. The word *defect* leaves normative text … Relax the v0.356 rule forbidding `IFailure` under a library "defect" class … Keep the cancellation exclusion …
> 4. Fix the §31.2.2 / Guide 2 §4 inconsistency and any other text v0.356 made over-claim …
> 5. Defer the caller-side "this failure is a bug here" conversion (record it with its reopening trigger: evidence from real programs). Checked arithmetic is a separate small proposal — do NOT write it now; record it as a follow-up.

**Owner decision, 2026-11-11, "Withdraw; OrNull is doctrine"** (QUOTED from the coordinator's relay): the owner observed that a type-naming `(try E on X)` is longer and less precise than the `*OrNull` method it was meant to replace — "It's a lot easier to type a method with *OrNull than it is to remember which exact exception type or types you want to swallow." §6 records it.

The decisions leave three things open: what becomes of `ParseRangeException` and `DomainFailedException`, whether the word *defect* survives anywhere, and whether the cancellation exclusion should stay. §3.4–§3.7 decide them, with reasons; §10 lists what is left for the owner.

## 2. The evidence

### 2.1 The note's measurement

MEASURED (the note's `data/contextuality.py` over `data/scenarios.tsv`; groups by the condition's v0.357 class, recomputed for this proposal):

| Group | Rows | A bug | Handled at the call | Fatal here, code right |
|---|---|---|---|---|
| environment and peer classes (I/O, network by origin, end of stream, time-out, channel) | 106 | **3** | 51 | 22 |
| `FormatException`, `SerializationException`, `ParseRangeException` | 66 | **8** | 32 | 12 |
| the classes v0.357 called defects | 89 | **37** | 35 | 6 |

Three readings of the table matter here:

- **Origin is stable for the environment and peer classes, and only for them.** 3 of 106 rows is a statement a library can make. The data classes' 8 of 66 is the majority of the 11 bugs v0.357 raised with a failure class (the note's §3.4, first row: a literal parsed, a shipped document, a migration's own assumption).
- **Disposition is not stable for any class.** No rule keyed on the exception type predicts more than 53% of the three-band dispositions; who supplied the operand predicts 70% (the note's §3.1). `FileNotFoundException` alone: 14 rows handled at the call, 4 reported, 8 fatal, 1 a bug (the note's §3.3). The owner's database file is one of the 8 fatal rows, and the class is right in every one of them; only the handler knows the file is required.
- **"Unmarked means the program is wrong" is false for about half the rows** (37 of 89), and stable only for `InvalidOperationException` (9 of 10). The note's §5.3 adds that 79% of the corpus arms naming a classified library class name a class v0.357 called a defect, and 99% of those complete handling.

### 2.2 The independent re-rating

The note's §2 asked for an independent relabelling before a proposal cites its rates. A separate critic lane rated a blind sample (`docs/research/failure-kind-by-context/data/rerating/`; method and limits in its README). MEASURED (`rerating/cmp.py` → `RESULTS.txt`):

| | |
|---|---|
| pairs (seed `20261111`, 30% per condition) | 100 |
| three-band κ | **0.66** (77% agreement) |
| bug-or-not κ | **0.89** (96% agreement) |
| confusion, rows the re-rater, columns the note (a, b, c) | a [20, 2, 1] · b [13, 22, 6] · c [1, 0, 35] |
| bugs in the sample's environment and peer rows | 0 of 29 (note: 1) |
| bugs in the sample's data-class rows | 5 of 22 (note: 7) |
| bugs in the sample's v0.357-defect rows | 17 of 35 (note: 16) |

The re-rater disagrees mostly on (a) against (b) — where the unit of work ends, the soft boundary the note's §2 names — and agrees on bug-or-not, the only fact a class could be asked to carry. Every headline reproduces in direction.

### 2.3 The inconsistency v0.357 carries

QUOTED, v0.357. Spec §31.2.2's example handles a defect class as expected input:

```text
var ex: ArgumentException when ex.ParamName = "amount" => Console.WriteLine("Invalid amount"),
InvalidOperationException => Console.WriteLine("Invalid state"),
```

and Guide 2 §4's first example does the same (`var e: InvalidOperationException => Log($"bad state: {e.Message}")`), while stdlib §1's band 1 says "A defect surface is never a recovery surface: its exception exists to report a bug, not to be branched on, and its class does not implement `IFailure`". The sentence predates v0.356 (v0.355 had its first clause); v0.356 bound it to the marker, which made the examples read as wrong. The note's §5.3 finds the same shape in the corpora. The sentence is the over-claim; the examples are what programs do.

## 3. The design

### 3.1 What the marker means

A class that implements `IFailure` states that **the outside world disagreed**: the environment produced the condition (a file system, a device, an input stream, the operating system), or a peer did (the other end of a channel, a connection, another domain), or the outside did not answer in time. That is a fact its author knows. It is also a **default for handlers** — a boundary that wants "what the outside did to me" names the marker. It is **never a disposition**: a missing database file is a failure, and the program that cannot run without it stops; catching it and stopping is a handler like any other.

A class that does not implement `IFailure` makes **no statement**. Its condition may be the program's, a user's, or anyone's; only the code that handles it knows which.

The rule the user learns is the note's, now true of the text:

> The class says who could have caused it. The call says whether you expected it. The handler says what happens next.

*Declined:* **no library class marked** (the note's option B: only the application marks). The environment and peer classes are the part of 0211 the data supports (3 of 106 rows a bug; 0 of 29 in the re-rating), a tool's top-level arm would otherwise have to list `IOException | TimeoutException | ChannelClosedException | …` by name, and a third-party library would face the same choice one level out. The owner kept the marker on these classes.

### 3.2 The taxonomy after v0.358

| | Classes |
|---|---|
| **implement `IFailure`** | `IOException`, `FileNotFoundException`, `DirectoryNotFoundException`, `EndOfStreamException`, `PathTooLongException` (stdlib §7.5); `TimeoutException` (§6.1); `ChannelClosedException`, `ChannelFullException` (§6.9); `DomainFailedException` (§6.8) |
| **make no statement** | every built-in class (`Exception`, `FormatException`, `ArgumentException`, `ArgumentNullException`, `ArithmeticException`, `OverflowException`, `DivideByZeroException`, `InvalidOperationException`, `DisposedAccessException`, `NotImplementedException`, `NullReferenceException`, `IndexOutOfRangeException`, `MatchFailureException`, `TypeInitializationException`, `ContractViolationException`); stdlib `SerializationException`, `ArgumentOutOfRangeException`, `KeyNotFoundException`, `DomainFaultedException` |
| **cancellation** | `OperationCanceledException` and every class under it |
| **fatal** | `StackOverflowException` (no arm matches it) |

Network: no landed class exists. Proposal 0007's `SocketException` derives from `IOException` and so is a failure by inheritance when it lands; the stdlib's text says a later class states its marker by the same test. *Permission*: a refused permission raises `IOException` (stdlib §7.2: "every other OS-level open failure"), so it is marked; there is no separate class.

Two judgement calls inside the narrowed set, kept as the owner's decision states them:

- **`ChannelClosedException` when the same code closed the channel** (the note's T33c, a bug). The other end of a channel is a peer even within one domain (0211 Q8 (b), kept), and the class is the normal end of a stream in T33a. The class cannot see who closed it; origin is still the peer's end.
- **`TimeoutException` for the program's own loop budget** (T38g, a bug). The class has no landed producer (stdlib §6.1, QUOTED: "provided for user libraries and future modules"); its producers raise it for a deadline outside the code that waits.

Unlanded proposals: of the classes 0211 §14 counted, `RegexParseException: FormatException` (proposal 0020) is no longer a failure by inheritance, which the data supports (a pattern is usually the program's literal); `InvalidDataException` and `DecompressionLimitException` (proposal 0069) derive from `IOException` and are, though their condition is the input data's. §10 Q3 puts that to the owner for 0069's graduation.

### 3.3 `FormatException` and `SerializationException`

Both lose the marker. Their condition is data in an external form, and the origin of the data is exactly what the library cannot see: `int.Parse(text)` cannot tell a port the user typed from `"8080"` in the program, and `Json.Parse<T>` cannot tell a request body from a schema file shipped with the service (the note's T21d, T16b, T37b). The everyday programs make the point from the other side: 26 of the 27 call sites that raise a v0.357 failure class from `Parse`, file calls or deserialization are sites where a raise would mean a bug or a broken installation (the note's §5.2; the programs carry their data inline, which inflates it). With the marker, an `IFailure` arm at a boundary answers those bugs as bad input.

A boundary that expects bad input names the class: `var e: FormatException => Respond(400, e.Message)`. That is one arm, and it is the boundary's statement, which is where the note's sample puts the decision.

*Declined:* **keeping the two classes marked** (v0.356's taxonomy, the note's option C part 1 alone, restating the words only). It keeps the majority of the bugs raised with a marked class (§2.1) and asks the restated text to call `FormatException` "the outside world's" when the outside is unknown.

### 3.4 `ParseRangeException`: retired

**Decided: retire it.** `int.Parse`, `Parse(s, radix)`, `double`/`float.Parse`, `decimal.Parse` and `Convert.To*` raise `OverflowException` for well-formed input that spells a value the type cannot hold, as they did at v0.355.

Reasons, in order of weight:

1. **The class existed only to carry the marker.** 0211 §4.4 item 1 and Q9 give its whole reason: out-of-range input was to be a failure without breaking the `OverflowException` arms around `Parse`. With the marker off `FormatException`, a marked `ParseRangeException` would put the two halves of one `Parse` contract on opposite sides of the marker: an `IFailure` arm would take `"99999999999"` and pass `"12x"`. The two conditions have one origin — whoever supplied the digits — so they take one answer.
2. **Kept as a plain `OverflowException` subclass, no handler would select on it.** The decline row *Checked enum conversion — a dedicated fault subclass rather than `OverflowException`* sets the bar ("no plausible handler branches enum-cast-vs-overflow"), and 0211 met it only through the boundary arm (QUOTED, 0211 §8: "a handler that branches on it exists, the boundary arm"). Without the marker that handler is gone, and the taxonomy "stays deliberately small" (the `NotSupportedException` decline row).
3. **Nothing depends on it.** No compiler source names it (MEASURED: `git grep ParseRangeException -- src tests` outside the rule corpus finds nothing); r40 raises `OverflowException`, so the retirement brings the spec back to what the compiler does; the 26 `OverflowException` arms over `Parse` in 13 test files (0211 §9.1) keep matching; tracker #8417's requirement disappears.

The note's 8 `ParseRangeException` rows hold no bug, which on its own would argue for keeping the marker; the sample is small and the condition is `FormatException`'s, so reason 1 decides. *Declined:* keeping the class as a plain subclass (reason 2), and keeping it marked (reason 1). Recorded as a `retired` row in `DECLINED.md`.

### 3.5 `DomainFailedException`: kept

**Decided: keep it, unchanged in shape.** `DomainFailedException: DomainFaultedException, IFailure` is what the join raises when the original exception's class implements `IFailure`. The owner's decision requires the kind to cross a join where the original was marked; a class's interfaces are fixed, so a subclass is still the only carrier (0211 §6.3). What changes is its population: the marked classes are now the environment and peer classes and every user class marked by its author — including a user class under `InvalidOperationException`, which v0.358 admits (rule-corpus case `s30_14_0020`).

With the base-chain rule gone (§3.6), `DomainFailedException` is no longer "a recorded exception" to a rule; it is an ordinary library class whose base is chosen so that every existing `DomainFaultedException` arm keeps matching every fault a join raises.

*Declined:* **marking `DomainFaultedException` itself** for every child fault, on the ground that the other domain is a peer. A child's bug is the same program's bug, and the original's own marker is the better statement; marking every join would make an `IFailure` arm take a child's `NullReferenceException`.

### 3.6 Who may conform, and the cancellation exclusion

**Decided.** Two rules remain:

1. Only an `Exception`-derived class may implement `IFailure` (unchanged; the dead-shape ground).
2. A class with `OperationCanceledException` in its base chain may not implement it, directly or through an interface extending it (kept).

The v0.356 rule that forbade the marker under every built-in or standard-library class that was not itself a failure is **removed**. A user class may be marked under `InvalidOperationException`, `ArgumentException`, `FormatException` or a class of its own, as suits its application: `class StaleOrder(message: string): InvalidOperationException(message), IFailure` is taken by an `IFailure` arm and by an arm on `InvalidOperationException`.

Why the removal: 0211 §3.3 gave two grounds. "A class may not contradict the kind of a class the documents declare" — under v0.358 an unmarked class declares nothing, so there is nothing to contradict. "It recreates the swallowing stdlib §4.8 keeps `KeyNotFoundException` clear of" — §4.8 is about the library choosing a base for its own class so that an arm takes it *unasked*; a user who derives `StaleOrder` from `InvalidOperationException` asks for exactly that. The cost 0211 accepted (31 of 1,187 corpus exception declarations rooted in such a class would have to be re-based to be marked, 0211 §3.3) is gone. 0211's own record anticipated this: "the strict direction can be relaxed later without breaking a program".

**Why the cancellation exclusion stays.** It was reconsidered, and no reason against it was found. Cancellation is the answer to a request the program itself made (§30.8): its propagation is how a cancelled task becomes `Canceled` and how `TaskScope` drains. A failure arm is typically a retry loop or `IFailure => ()`, and a marked cancellation would be retried or swallowed — the program's own request to stop would be defeated by its own handler. That is a correctness hazard that does not depend on what any class "means", which is why it survives a change that removes every statement about meaning. The one argument for lifting it — a user wanting a cancellation subclass taken at a boundary — is served by naming `OperationCanceledException` in that arm. *Declined:* lifting the exclusion.

### 3.7 The word *defect*

**Decided: it leaves the normative text, and is not kept as guide vocabulary either.** The spec, the stdlib and the rationale no longer name a kind for unmarked classes; the guides, the Tour and the code-generation reference say "bug" where they mean a bug in a specific situation, and "makes no statement" where they mean an unmarked class.

Reasons: a category word for unmarked classes invites exactly the reading §2.1 refutes — that the absence of the marker says something. *Unclassified* (the note's alternative) would be accurate but is a word for nothing. Guide vocabulary is copied by readers and models (the code-generation reference is written to be copied from), so a word kept "only in the guides" would be re-imported into code comments and handlers as a class property. The other uses of *defect* in the documents ("a defect in this specification", "the two-owners defect", "a defect in the *contract*") are ordinary English and are untouched. *Declined:* *unclassified*, and keeping *defect* as informal guide vocabulary.

*Fault* keeps meaning "an exception was raised" (0211 Q1's decline of *fault* for the bug band stands, now moot).

### 3.8 What does not change

The marker's shape (a member-less interface, 0211 Q2); the catch-binding rule (Q3) and the recorded difference from `case` (Q4); the domain join's mechanism; the unreachable-arm rule over interfaces; Appendix E's row (adding the marker is breaking; removing it is breaking by the row above — §8.1 says why the removal here breaks no program); ownership, cleanup, supersession, async faults, purity and the FFI abort, for every exception alike; the failure-signal rule's five bands and the no-dual rule; the throwing tier's stance gloss ("this call site handles nothing: it asserts the operation succeeds"). The v0.355 glosses ("failure is a bug at this call site") are **not** restored: they state a disposition, which v0.358 leaves to the handler. Stdlib §2.5's band label for the `DateTime` range fault stays band 1 ("a band-1 condition of the arithmetic domain"); its reconsideration goes with the checked-arithmetic follow-up (§7.2), whose query would change the "no query" premise 0211 Q7 recorded.

## 4. The over-claims removed

Each found by the grep the brief names ("defect", "never a recovery surface", "the program is wrong", "a bug at", `IFailure`, "failure band", the stdlib §1 rule and its band glosses) over the eleven documents and the Tour, and each edited in §9:

| Text (v0.357) | Why it over-claims | Edit |
|---|---|---|
| spec §31.4: "An exception is of one of two kinds, and the kind is a property of its class … Every other exception is a **defect** report: the program is wrong" | false for 37 of 89 rows (§2.1) | S7 |
| spec §31.1: "every other is a **defect** report" | the same | S2 |
| spec §31.4: "a class that does not list the marker declares a defect report" | the same | S10 |
| spec §31.2.4–§31.2.5, Appendix C.7: "defects pass", "every defect class" | the same | S3, S4, S14 |
| stdlib §1 band 1: "A defect surface is never a recovery surface: its exception exists to report a bug, not to be branched on" | contradicted by spec §31.2.2's and Guide 2 §4's examples (§2.3) and by the corpora | L2 |
| stdlib §1 band 4: "a typed exception whose class implements `IFailure`"; "what separates this band from band 1 is where the condition comes from" | band 4 holds data conditions now unmarked; v0.355's sentence returns | L3 |
| stdlib §1 *Failures and defects*: the band fixes the kind; the origin test that makes text "the outside's" | the data half is refuted (§3.3); the misfits it needed go with it | L4 |
| stdlib §1 no-dual rule: "the throwing `Parse` raises a failure" | `FormatException` is unmarked | L5 |
| stdlib §2.6, §2.14, §2.18, §3.1, §3.2: six "band-1 defect raised with a failure class" misfit flags | no misfit remains | L8, L9, L11–L13 |
| rationale §11.4: "The line between the rule's first and fourth bands is visible in the exception's class" | band 4 is not all marked | R1 |
| rationale §11.12: "Unmarked means defect"; "a kind is the condition's" with `Parse` a failure | §2.1, §3.3 | R2–R8 |
| Guide 2 §4: "Every exception is one of two kinds"; the arm comment "expected: answered"; "Wrap a failure in a failure and never wrap a defect" | states a disposition; teaches the refuted halves | G2-2 to G2-9 |
| Guide 6 §2: "What it raises is a failure either way … a boundary's `IFailure` arm takes both"; "a defect report that a boundary's `IFailure` arm lets pass" | `FormatException` is unmarked; the defect gloss | G6-1 to G6-5 |
| the Tour §6: "of two kinds … a *defect* report, which is a bug" | the same | T1 |
| the code-generation reference §5.1, §5.2, §6: "a *failure* where … the outside's — `Parse` … a *defect* where …", the *Kinds* line, "an unmarked class is a defect report" | the same | C1–C6 |

The first row of the table answers the brief's item 4: spec §31.2.2's example and Guide 2 §4's `InvalidOperationException => Log(…)` arm are now consistent with every normative sentence, because no sentence any longer says an unmarked class may not be handled. The examples are kept as written.

## 5. Recorded decisions reopened

Process §2: "A change is a design change the moment it contradicts a recorded decision … reopened only with *new evidence against the stated reasons*". Each row names the decision, the stated reason, and the new evidence.

| Decision | Stated reason (QUOTED) | New evidence | v0.358 |
|---|---|---|---|
| **0211 §16 Q1** — *defect* as the word for the bug band | "*Fault* keeps meaning 'an exception was raised'… *defect* names the bug band" | there is no bug band to name: unmarked classes are a bug in 42% of rows (§2.1), 49% in the re-rating (§2.2) | **Overtaken.** No word names unmarked classes (§3.7); *fault*'s meaning is kept |
| **0211 §16 Q5 (b)**; `DECLINED.md` *A user exception class listing `IFailure` under a built-in or standard-library class that is not a failure … including the narrower rule that forbids only cancellation subclasses* | "it recreates the swallowing stdlib §4.8 keeps `KeyNotFoundException` clear of"; "keeps the documents' own classification true" | the classification it protected is withdrawn — an unmarked class states nothing to contradict (§2.1); §4.8's hazard is a library choosing a base unasked, not a user asking | **Reversed in part.** The defect-class leg is removed; the cancellation leg — the narrower rule the row declined as *insufficient* — is now the whole rule (§3.6) |
| **0211 §16 Q6**; `DECLINED.md` *The exception kind following the tier* | "One class has one kind: `FormatException` is both what `int.Parse` raises and the input failure Guide 6's `Listen` throws. The kind is the condition's origin" | the conclusion (the tier does not decide the marker) stands; its example does not: text is not "the outside's" when its supplier is unseen (8 of 66 bugs; 26 of 27 everyday sites assert-stance, the note's §5.2) | **Ground restated.** The marker is the condition's, not the tier's; `Parse` raises the unmarked `FormatException` whatever the stance |
| **0211 §16 Q7**; `DECLINED.md` *Arithmetic over external data … as a failure* | "the operands are typed values the program already holds" | the classification needs no defence now — arithmetic is neither the environment's nor a peer's — but the note finds 10 of 13 overflow rows are expected results of the data | **Ground restated; outcome kept.** `OverflowException` is unmarked; the reopening bar's second leg ("a standard-library checked-arithmetic query surface") becomes the recorded follow-up (§7.2) |
| **0211 §16 Q8 (a)** — a missing key, an empty collection and an index out of range "are defects" | "the program owns the state tested" | 21 such rows are the user's, a peer's or a host guest's key, index or data (the note's §5.5) | **Restated.** The classes are unmarked and make no statement; the query forms remain the expected-absence path |
| **0211 §16 Q8 (c)** — `TypeInitializationException` "a defect whatever its inner exception" | "nothing a handler does recovers it" | none against the reason | **Kept in effect**: unmarked whatever its inner exception |
| **0211 §16 Q9**; `DECLINED.md` *A clean cut in place of the two bridging classes* | "26 `OverflowException` arms … would stop matching; the two bridges keep every arm taking what it took" | `ParseRangeException`'s only selector, the marker, is gone (§3.4 reason 2); retiring it raises `OverflowException`, so every arm still matches | **`ParseRangeException` retired; `DomainFailedException` kept.** The row is moot in its first half: no arm flips |
| **0211 §16 Q10**; `DECLINED.md` *Re-typing the six recorded failure-class misfits* (`deferred`) | "a kind cannot be shed … this landing was the cheapest moment there will be" | the misfits were defect conditions raised with `FormatException`/`SerializationException`; both classes are unmarked | **Moot.** No misfit remains, and no arm changes |
| **0211 §16 Q12**; `DECLINED.md` *"Unmarked means failure"* | "A boundary arm would swallow whatever a library author forgot to classify" | none against the reason; the row's phrasing ("the default kind") assumes a kind | **Kept**, reworded: unmarked makes no statement, and an `IFailure` arm takes only marked classes |
| `DECLINED.md` *A base class `Failure: Exception` as the failure marker* | "the out-of-range `Parse` input and the domain join could not join the band without leaving the classes existing arms name" | one bridge remains; v0.358 lets a user class be marked under any base, which a base class cannot express at all | **Ground restated and strengthened**; the decline stands |
| `DECLINED.md` *D-0005, value-form try expression*; the error-model note's §8.5 stage 2 | "it makes exception-based *expected* failure ergonomic" | the owner's ruling of 2026-11-11 (§6): a caller-named type list is longer and less precise than the dual; a default set swallows what the caller did not mean | **Not reopened; the direction is withdrawn**, with a recorded bar (§6) |

Untouched: 0211 Q2, Q3, Q4, Q11 (proposal 0213), Q13, Q14, and the rows *Reporting members on `IFailure`*, *The kind as a virtual property*, *Keeping the binding at the interface type*, *A kind field on `ExceptionInfo`*, *Re-raising a defect ahead of a failure*, *Abandoning the process or the domain on a defect*, *Staging the marker before the join*. Proposal 0213's design exempts an `Exception` arm after an `IFailure` arm; under v0.358 that arm takes less, so 0213 re-reads its exemption at its own graduation (its trigger is unchanged).

## 6. The call-site `(try E)` form: withdrawn

The error-model note's §8.5 stage 2 (an atom that evaluates `E` and yields `null` for some exceptions) was gated, and 0211 §7 kept the marker compatible with it. The owner withdrew the direction on 2026-11-11 ("Withdraw; OrNull is doctrine"). It is recorded here because 0211 described the marker as the atom's predicate, and the narrowing changes that predicate.

**Decided.** The `(try E)` form is **withdrawn as a direction**, not deferred. The `*OrNull` and `Try` tiers (stdlib §1's Try convention and no-dual rule) are the doctrine for value-shaped results: the author of a member, who knows its expected-absence condition, states it once by providing the dual. That is more precise than either alternative:

- **Declined: `(try E)` over a default set** — every `IFailure`, or any fixed set. It swallows conditions the caller never meant to tolerate (a `try` around a call that reads a file and parses it would turn a disk error into `null` beside the parse failure the caller meant), and after the narrowing it would not even cover `FormatException`, the case it was for. Swift's `try?` and Kotlin's `runCatching` are the cautionary precedents: each converts every error, and `runCatching` is widely criticised for catching `CancellationException` and breaking structured cancellation (HYPOTHESIS: prior art from memory, no source consulted).
- **Declined: `(try E on X)`**, naming the types at the call. It is longer than the dual (`(try int.Parse(s) on FormatException | OverflowException)` against `int.ParseOrNull(s)`), and less precise: the caller must remember which exception types each member raises for its expected condition, and a list that is right today is wrong when a member gains a condition. The owner, QUOTED: "It's a lot easier to type a method with *OrNull than it is to remember which exact exception type or types you want to swallow." Python's `contextlib.suppress(FileNotFoundError)` is this shape (HYPOTHESIS: from memory), and is a statement-level helper, not an expression that produces a value.

**Reopening bar** (recorded in `DECLINED.md`): the only shape a future proposal may bring is a **declared-absence form** — each throwing member declares, once, which of its exceptions means "no result", and a call-site form reads that declaration, so that it is exactly as precise as the `*OrNull` dual it replaces. Such a proposal must answer §31.3's *No `throws` clause* decision, since a per-member exception declaration is that clause's neighbour, and must show that it removes enough duals to pay for new declaration syntax (the error-model note counted about 31 `Parse`-family duals, §8.5 stage 3).

**A missing dual is a library gap.** Where a member documents an expected failure and offers no value-shaped form, the remedy is the dual, not a language form. The error-model note's scan found 33 of 82 stdlib sections naming an expected failure, 15 of them with no value-shaped form — all of `System.IO`, serialization and `Encoding` among them (MEASURED, `docs/research/error-model/data/stdlib_stance_gap.json`, stdlib v0.355). That is filed as [#8516](https://github.com/s8lap/bezel-lang/issues/8516) (`stdlib`, `parking-lot`), one issue listing the sections, with the scan's false positives named.

0211 §7's text is history and stays; 0211's status row gains a pointer to this section.

## 7. Deferred, and follow-ups

### 7.1 The caller-side conversion: deferred

The note's §7 part 2 — a one-token way for a caller to say "a failure at this call is a bug" (a shipped schema file, a required database file opened beneath a request boundary) — is **deferred**. Today's idiom is a catch-and-wrap of a few lines:

```bezel
public class ShippedFileMissing(message: string, cause: Exception): Exception(message, cause)
end ShippedFileMissing;
```

thrown from an `IFailure` arm around the open. Under v0.358 the need is smaller than the note measured: `FormatException` and `SerializationException` no longer carry the marker, so of the note's 11 bugs raised with a failure class only the environment and peer rows remain (3 of 106), and 4 of the 9 at-risk rows of the note's §3.6 left the marked set. **Reopening trigger:** evidence from real programs written against a compiler that implements the marker — boundaries whose `IFailure` arm answers a required resource's absence, or another bug, as routine, often enough that the hand-written wrap is a measurable cost (the same programs proposal 0213's trigger names). Recorded as a `deferred` row.

### 7.2 Checked arithmetic: a separate follow-up

Not written here. The note finds 10 of 13 overflow rows are expected results of the data being computed (an attacker chooses the magnitudes in two), and the library offers no query: no checked add that returns `null`. A small standard-library proposal should add nullable-returning checked-arithmetic members on the integer types (the stdlib §1 `*OrNull` tier) and a validity query for date components, and with them reconsider stdlib §2.5's band label (§3.8). Proposal 0008 (*Integer toolkit*, accepted) already designs `Checked*` named methods; the follow-up starts there. Recorded as a `deferred` row whose bar is that proposal.

### 7.3 Other follow-ups

- **#8516**: the value-shaped forms the stdlib lacks (§6).
- **#8426** (the `Weak` never-strongly-reached condition): dissolved. The question was whether a read-side `SerializationException` condition is a misfit; with the class unmarked there is no misfit to count. Commented on the issue.
- **Guide 4** (#8427): re-traced in full against v0.358, which found one stale passage outside this proposal's subject — §8 still listed §22.12's mutable-static form (iv), retired at v0.334 — and fixed it (G4-2, G4-3). Its pin moves from v0.331.

## 8. Migration and conformance

### 8.1 What programs see

No compiler implements `IFailure`, `ParseRangeException` or `DomainFailedException` (MEASURED: `git grep -e IFailure -e ParseRangeException -e DomainFailedException -- src tests ':!tests/conformance/rules'` finds nothing; r40 rejects each name as unresolved, tracker #8417). No program outside the rule corpus names the marker (0211 §9.1). So:

- *No longer rejected:* a class that lists `IFailure` under a built-in or standard-library exception class other than `OperationCanceledException` (`class StaleOrder: InvalidOperationException, IFailure`).
- *No longer defined:* `ParseRangeException`; `IFailure` on `FormatException` and `SerializationException`.
- *Behaviour changes, every arm still matching:* out-of-range `Parse`/`Convert.To*` input raises `OverflowException` (an `OverflowException` arm took `ParseRangeException` and takes this). An `IFailure` arm no longer takes `FormatException`, `SerializationException` or out-of-range input. Appendix E classifies removing the marker as breaking; nothing outside the rule corpus observes it, since no implementation has shipped it.
- *Newly warned:* nothing. An arm on `FormatException` after an `IFailure` arm, which v0.356 made unreachable, is reachable again (no warning).
- *Doc-only:* every restatement of §4; the misfit flags removed; the guides, the Tour and the reference.

### 8.2 Rule corpus (process §5 item 3c)

Swept by text and construct (`manifest.tsv` grepped for `IFailure`, failure, defect, `FormatException`, `SerializationException`, `ParseRangeException`, `DomainFailed`, and the binding and who-may-conform rules: 54 hits, of which 51 name the marker or the classes). MEASURED counts after `corpus.py build`: 5,849 → 5,857 cases.

| Change | Cases |
|---|---|
| **verdict changes** (8) | `s31_4_0021`, `s31_4_0023` (reject → accept: a marker under `InvalidOperationException`, and under a user base deriving from `ArgumentException`); `s31_4_0012`, `l2_15_0039`, `l2_3_0033`, `l9_1_0030`, `l9_2_0020`, `l9_3_0022` (an `IFailure` arm took the exception; it now passes, and the class's own arm takes it) |
| **program rewritten, verdict kept** (4) | `s31_2_3_0022` (warn) and its twin `s31_2_3_0023` (quiet): the unreachable-arm pair moves from `FormatException` to `EndOfStreamException`, a marked class; `l2_5_0061`, `l9_1_0031`: the outer arm no longer tests `is ParseRangeException` |
| **rule quote or title refreshed, verdict kept** (24) | `l1_0007`, `l1_0008`, `l2_3_0002`, `l2_3_0023`, `l6_9_0014`, `l6_9_0015`, `l9_1_0007`, `l9_2_0007`, `l9_2_0008`, `l9_3_0020`, `s22_16_0032`, `s30_9_0010`, `s30_14_0018`, `s31_2_0014`, `s31_2_5_0002`, `s31_4_0009`, `s31_4_0010`, `s31_4_0011`, `s31_4_0013`, `s31_4_0019`, `s31_4_0020`, `s31_4_0022`, `s31_4_0024`, `s31_4_0025` (in seven of them the program's printed word "defect" is also replaced) |
| **new** (8) | `s31_2_3_0024` (quiet: a `FormatException` arm after an `IFailure` arm is reachable — `FormatException` is not marked; twin of `s31_2_3_0022`); `s31_4_0026` (run: a marked subclass of `InvalidOperationException` is taken by an `IFailure` arm and by its base's arm; a plain `InvalidOperationException` passes); `s31_4_0027`/`s31_4_0028` (reject/accept twins: the marker under a user base deriving from `OperationCanceledException`); `s31_4_0029`/`s31_4_0030` (reject/accept twins: an interface extending `IFailure` under `OperationCanceledException`, and under `InvalidOperationException`); `l6_1_0011` (run: `TimeoutException` is taken by an `IFailure` arm — an environment class still is); `s30_14_0020` (run: a marked subclass of `InvalidOperationException` raised in a child is joined as `DomainFailedException`) |
| **unchanged, re-read** | `l1_0001`, `l6_12_0001`, `l6_12_0006`, `l7_5_0003`, `s30_14_0017`, `s30_14_0019`, `s31_2_1_0010`–`0012`, `s31_2_1_0017`, `s31_2_1_0018`, `s31_4_0014`–`0018`, `l5_0010` and `s28_6_0040` (whose "defective"/"unmarked" are ordinary words) |
| deleted | none: every rule v0.358 changes still has an isolable program |

No banked case changes verdict: the eight flips were unbanked v0.356 cases; `l2_3_0002` and `l9_1_0007` (banked `pass`) keep their verdicts. `baseline.tsv` is not touched.

### 8.3 Documents

Example-extraction parity runs clean (`check_docs.sh`). The three fenced examples that change — Guide 2 §4's `Serve`, the reference §6's boundary idiom and Guide 6 §2's `Listen` comment — were traced on every path: `Serve` returns `BadRequest` for a `FormatException`, `BadGateway` for a marked class, and reports anything else; `Response.BadRequest` and `ReportBug` are illustrative names of the program's own, as `Response.BadGateway` and `ReportDefect` were.

### 8.4 Trackers (process §5 items 3, 4)

Every v0.358 spec-ahead-of-impl delta is a change to a requirement an existing tracker already carries, so none is filed new (deduplicated against the five v0.356 trackers):

- [#8414](https://github.com/s8lap/bezel-lang/issues/8414) (the marker and the library conformances): the conformance list shrinks — `FormatException` and `SerializationException` no longer implement it.
- [#8416](https://github.com/s8lap/bezel-lang/issues/8416) (who may conform): the base-chain rejection narrows to `OperationCanceledException`.
- [#8417](https://github.com/s8lap/bezel-lang/issues/8417) (`ParseRangeException`): void — the class is retired and r40 already raises `OverflowException`; to close as not planned when v0.358 lands.
- [#8418](https://github.com/s8lap/bezel-lang/issues/8418) (`DomainFailedException`): unchanged in requirement; its population now includes user classes marked under any base.
- [#8415](https://github.com/s8lap/bezel-lang/issues/8415) (the binding rule): unchanged.

Filed: [#8516](https://github.com/s8lap/bezel-lang/issues/8516) (§6). Commented: #8407, #8426, #8427.

## 9. The normative edits

Machine-checkable twin: [`docs/research/failure-kind-by-context/data/edits_0214.py`](../research/failure-kind-by-context/data/edits_0214.py) (`check` against v0.357: 78 edits over 9 documents, 0 anchor failures; `apply` reproduces the landed documents byte for byte; `landed` checks every new text is present). The changelog entry and the proposals records are not in the edit set. Each `Old` occurs exactly once in its document as the edits before it leave it.

**S1** — `Bezel_Language_Spec.md`, §30.14 *Faults*.

Old:

````text
The exception's kind crosses with them: where the original exception's class implements `IFailure`
````

New:

````text
The failure marker crosses with them: where the original exception's class implements `IFailure`
````

**S2** — `Bezel_Language_Spec.md`, §31.1 *The throw operand*, the last two sentences.

Old:

````text
A throwable whose class implements `IFailure` is a **failure**; every other is a **defect** report, cancellation apart ([§31.4](#sec-the-failure-marker)). Dispatch reads the kind only where an arm names the marker.
````

New:

````text
A throwable whose class implements `IFailure` is a **failure**: the environment or a peer produced it ([§31.4](#sec-the-failure-marker)). A class that does not implement it makes no statement about where its condition came from. Dispatch reads the marker only where an arm names it.
````

**S3** — `Bezel_Language_Spec.md`, §31.2.4, the last example's comment.

Old:

````text
  var f: IFailure => Log(f)                 // every other failure; `f` is an `Exception` (§31.2.1); defects pass
````

New:

````text
  var f: IFailure => Log(f)                 // what the environment or a peer raised; `f` is an `Exception` (§31.2.1); the rest propagates
````

**S4** — `Bezel_Language_Spec.md`, §31.2.5, the last sentence.

Old:

````text
To suppress expected failures only, name the marker — `IFailure => ()` — and defects and cancellation still propagate ([§31.4](#sec-the-failure-marker)).
````

New:

````text
To suppress only what the environment or a peer raised, name the marker — `IFailure => ()` — and every other exception, cancellation included, still propagates ([§31.4](#sec-the-failure-marker)).
````

**S5** — `Bezel_Language_Spec.md`, §31.4, the first paragraph.

Old:

````text
(and `ArgumentException`/`ArgumentOutOfRangeException`'s `(message, paramName)` forms, and the `()` and `(message)` forms of `ParseRangeException`, stdlib [§9.1](Bezel_Standard_Library.md#sec-integer-types)) are **`pure`**
````

New:

````text
(and `ArgumentException`/`ArgumentOutOfRangeException`'s `(message, paramName)` forms) are **`pure`**
````

**S6** — `Bezel_Language_Spec.md`, §31.4, the tree's `FormatException` line.

Old:

````text
├─ FormatException                  — string does not conform to an expected format. A failure: implements `IFailure`.
````

New:

````text
├─ FormatException                  — string does not conform to an expected format.
````

**S7** — `Bezel_Language_Spec.md`, §31.4 *The failure marker*, the first paragraph.

Old:

````text
**The failure marker.** An exception is of one of two kinds, and the kind is a property of its class. A **failure** reports a condition a correct program can meet: the environment, input from outside the program, a peer, or a time or capacity limit produced it. Its class implements the built-in interface `IFailure`. Every other exception is a **defect** report: the program is wrong, and the exception says where. One class is named apart: `OperationCanceledException` reports **cancellation**, the answer to a request the program itself made; it does not implement `IFailure`, and a handler of failures never takes it.
````

New:

````text
**The failure marker.** An exception whose class implements the built-in interface `IFailure` is a **failure**: the outside world disagreed. The environment produced the condition (a file system, a device, an input stream, the operating system), or a peer did (the other end of a channel, a connection, another domain), or the outside did not answer in time (a deadline). That is a statement of origin, which the class's author can make, and a default for handlers. It is not a disposition: whether a program carries on, reports, retries or stops when a failure reaches it is the decision of the handler that takes it, and a failure the program cannot do without — a file it was installed with, the server it exists to call — is still a failure, and the program answers it by stopping. A class that does not implement `IFailure` makes **no statement**: its condition may come from the program, from data the program was given, or from anywhere else, and only the code that handles it knows which. One class is named apart: `OperationCanceledException` reports **cancellation**, the answer to a request the program itself made; no class in its tree implements `IFailure`, and a handler of failures never takes it.
````

**S8** — `Bezel_Language_Spec.md`, §31.4 *The failure marker*, the second paragraph.

Old:

````text
so a kind is inherited and cannot be shed.
````

New:

````text
so the marker is inherited and cannot be shed.
````

**S9** — `Bezel_Language_Spec.md`, §31.4 *Who may conform*, after the first restriction.

Old:

````text
And a class does not contradict the kind of a class the documents declare: a class that does not inherit `IFailure` may list it only where no class in its base chain, other than `Exception` itself, is a built-in or standard-library exception class; listing it otherwise is a compile error at the declaration. So a class deriving from `OperationCanceledException` is cancellation in every subclass, and an arm naming `InvalidOperationException`, or any other defect class of the documents, never takes a failure that user code declared. A class whose base chain is user-declared down to `Exception` chooses freely at each level. The standard library declares the only two classes that derive across that line, each by recorded ruling (stdlib [§1](Bezel_Standard_Library.md#sec-failure-signal-rule)). An interface may extend `IFailure`; both restrictions then bind its implementers.
````

New:

````text
And a class with `OperationCanceledException` in its base chain may not implement it: listing `IFailure`, or an interface extending it, on such a class is a compile error at the declaration, so cancellation is cancellation in every subclass. Every other `Exception`-derived class may list it, whatever its base: a class deriving from `InvalidOperationException`, from `FormatException` or from a class of the program's own is a failure where its author says so, and an arm naming its base still takes it. An interface may extend `IFailure`; both restrictions then bind its implementers.
````

**S10** — `Bezel_Language_Spec.md`, §31.4, the paragraph after *Who may conform*.

Old:

````text
Of the built-in types, `FormatException` implements `IFailure`. The standard library's failure types, and the rule by which a condition is raised as a failure or as a defect, are stated in stdlib [§1](Bezel_Standard_Library.md#sec-failure-signal-rule). A class that this specification or the standard library declares changes kind only by recorded ruling. User code chooses per class: `class ConfigError(message: string): Exception(message), IFailure` declares a failure, and a class that does not list the marker declares a defect report.
````

New:

````text
No built-in type implements `IFailure`. The standard library's failure types, and the rule by which the library marks a class, are stated in stdlib [§1](Bezel_Standard_Library.md#sec-failure-signal-rule). Whether a class this specification or the standard library declares implements the marker changes only by recorded ruling. User code chooses per class: `class ConfigError(message: string): Exception(message), IFailure` declares a failure, and a class that does not list the marker makes no statement.
````

**S11** — `Bezel_Language_Spec.md`, §31.4, the paragraph on what reads the marker.

Old:

````text
The kind is read in two places. A catch arm that names the marker selects on it: an `IFailure` arm takes every failure and lets defects and cancellation pass ([§31.2.3](#sec-catch-ordering-and-exhaustiveness)).
````

New:

````text
The marker is read in two places. A catch arm that names it selects on it: an `IFailure` arm takes every failure and lets every other exception, cancellation included, pass ([§31.2.3](#sec-catch-ordering-and-exhaustiveness)).
````

**S12** — `Bezel_Language_Spec.md`, §31.4, the same paragraph, its last sentence.

Old:

````text
and the reports of [§32.8](#sec-callbacks) and [§33.3](#sec-exit-code) are the same for both kinds and for cancellation.
````

New:

````text
and the reports of [§32.8](#sec-callbacks) and [§33.3](#sec-exit-code) are the same for a failure, for an exception whose class makes no statement, and for cancellation.
````

**S13** — `Bezel_Language_Spec.md`, §31.4, the last paragraph (the stdlib exception types).

Old:

````text
`ParseRangeException` (deriving from `OverflowException` and implementing `IFailure`; raised by the `Parse` family for a value outside the type's range; declared in the auto-imported `System`, stdlib §9.1), 
````

New: *(deleted)*

**S14** — `Bezel_Language_Spec.md`, Appendix C.7, the `IFailure` bullet.

Old:

````text
and so is a class that does not inherit it and has a built-in or standard-library exception class other than `Exception` in its base chain — `OperationCanceledException` and every defect class among them ([§31.4](#sec-the-failure-marker)).
````

New:

````text
and so is a class with `OperationCanceledException` in its base chain ([§31.4](#sec-the-failure-marker)).
````

**S15** — `Bezel_Language_Spec.md`, Appendix E, the `IFailure` row.

Old:

````text
The kind is what a consumer's boundary arm selects on ([§31.4](#sec-the-failure-marker)): adding the marker makes every `IFailure` arm take the class.
````

New:

````text
The marker is what a consumer's `IFailure` arm selects on ([§31.4](#sec-the-failure-marker)): adding it makes every such arm take the class.
````

**L1** — `Bezel_Standard_Library.md`, §1, the three access tiers.

Old:

````text
asserts the value is there and raises where it is not (a failure or a defect by where the condition comes from — *Failures and defects*, below)
````

New:

````text
asserts the value is there and raises where it is not (the class raised is the condition's, whatever the caller's stance — *The failure marker*, below)
````

**L2** — `Bezel_Standard_Library.md`, §1, band 1.

Old:

````text
the residual defect exceptions (such as `Argument*` and `InvalidOperationException`) at pre-contract boundaries, and the arithmetic classes of language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types) (`OverflowException`, `DivideByZeroException`) where a member surfaces an arithmetic fault. A defect surface is never a recovery surface: its exception exists to report a bug, not to be branched on, and its class does not implement `IFailure` (*Failures and defects*, below).
````

New:

````text
the residual exceptions (such as `Argument*` and `InvalidOperationException`) at pre-contract boundaries, and the arithmetic classes of language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types) (`OverflowException`, `DivideByZeroException`) where a member surfaces an arithmetic fault. The band fixes what the library offers, not what a handler does: a caller that expects the condition at its call tests first, with the query or tester form beside the operation where one exists, and a handler that catches one of these exceptions decides what it means, as for every exception. None of these classes implements `IFailure` (*The failure marker*, below).
````

**L3** — `Bezel_Standard_Library.md`, §1, band 4.

Old:

````text
→ a typed exception whose class implements `IFailure` (a **failure**; *Failures and defects*, below), producer-local per language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types), **regardless of how much information it carries**, with the classification and its reason recorded beside the surface. The typical inhabitant is the environmental fault (I/O, network, OS state). What separates this band from band 1 is where the condition comes from, the test stated once in *Failures and defects* (below). This band's wording about where the fault is consumed does not replace that test.
````

New:

````text
→ a typed exception, producer-local per language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types), **regardless of how much information it carries**, with the classification and its reason recorded beside the surface; its class implements `IFailure` where the environment or a peer produces the condition (*The failure marker*, below). The typical inhabitant is the environmental fault (I/O, network, OS state); environmental origin describes the band, it is not a conjunct of the test.
````

**L4** — `Bezel_Standard_Library.md`, §1, *Failures and defects*, replaced by *The failure marker*.

Old:

````text
**Failures and defects.** The exceptions this library raises are of the kinds language spec [§31.4](Bezel_Language_Spec.md#sec-the-failure-marker) defines, and the band fixes the kind: a band-4 condition is raised with a class that implements `IFailure`, a band-1 condition with one that does not, and cancellation with `OperationCanceledException`. Bands 2 and 3 have value shapes as their mechanism; where a member also offers a throwing tier (the no-dual rule, below), the throwing tier of a band-2 or band-3 condition takes the kind its origin gives: end of input at `Console.ReadKey` is the environment's, and a failure; an absent key at the indexer is the program's, and a defect. The test, between bands 1 and 4 and for every throwing tier, is **where the condition comes from**. It comes from outside the program when the environment produces it (a missing file, a failed write, the end of an input stream), when it is found in data in an external form that a member is reading into program values (text that does not parse, bytes that do not decode, a document that does not deserialize), when a peer produces it (a closed channel, a failure in another domain), or when a limit does (a deadline, a full buffer). A correct program can meet every one of these, and the condition is a failure. It comes from the program itself when it is a fact about what the program passed (an index, a key, a count, a radix, a format specifier), about the state of an object or collection the program holds (an empty collection, an absent key, a closed scope, a live enumeration), or about its own arithmetic. The program owns that state and can know it before the call, so the condition is a defect, however the values in it were arrived at. Two lines are drawn once, here. What makes data the outside's is its form: text or bytes that a member is reading into a program value. A conversion from one program value to another — an enum cast, a checked narrowing — tests a value the program already holds, and is the program's. And the other end of a channel is a peer even within one domain: a sender does not hold the receiving end's state, nor the receiver the senders', so `ChannelClosedException` is a failure whichever party closed the channel; a `TaskScope` has one owner, and spawning into a scope that owner has closed is a fact about state it holds. The kind is the condition's and not the tier's (the no-dual rule, below): `Parse` and the `[key]` indexer are both throwing forms with a query beside them, and `Parse` raises a failure, because the text is the outside's, where the indexer raises a defect, because the dictionary is the program's. The failure types this document declares are `IOException` and the classes deriving from it ([§7.5](#sec-io-exception-types)), `SerializationException` ([§2.15](#sec-concrete-serialization-formats)), `TimeoutException` ([§6.1](#sec-cancellationtoken-and-cancellationtokensource)), `ChannelClosedException` and `ChannelFullException` ([§6.9](#sec-channel)), `ParseRangeException` ([§9.1](#sec-integer-types)) and `DomainFailedException` ([§6.8](#sec-domain)); with the built-in `FormatException` they are the library's whole failure band. One exception class is of one kind, and a member does not raise a failure with a defect class or a defect with a failure class. No class of this library that implements `IFailure` derives from one that does not, with two recorded exceptions, each keeping an established catch arm matching: `ParseRangeException` derives from `OverflowException`, and `DomainFailedException` from `DomainFaultedException`. A landed member that raises a failure class for a band-1 condition is a recorded misfit under this rule's scoping sentence, flagged where it stands: the unrecognized format specifier ([§2.18](#sec-iformattable), [§2.6](#sec-guid), [§3.1](#sec-stringbuilder)), the write-side serialization conditions ([§2.14](#sec-serialization-vocabulary), [§2.15](#sec-concrete-serialization-formats)) and the unrepresentable code point at `GetBytes` ([§3.2](#sec-encoding)). A kind is inherited and cannot be shed (language spec §31.4), so moving such a condition means raising it with a class outside the failure class's subtree, and the arms naming the failure class stop taking it. A class's kind changes only by recorded ruling, as the taxonomy grows only by one (language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)).
````

New:

````text
**The failure marker.** The exceptions this library raises carry the marker of language spec [§31.4](Bezel_Language_Spec.md#sec-the-failure-marker) by **origin**. A class implements `IFailure` exactly when its condition is the outside world disagreeing: the **environment** produced it (a missing file, a refused permission, a failed write, the end of an input stream), or a **peer** did (the other end of a channel closed it or let its buffer fill — a peer even within one domain, whichever party closed the channel; a computation in another domain failed), or the outside did not answer in time (a deadline). A class whose condition can come from the program itself, or from data whose supplier the library cannot see, is not marked, and makes no statement: text that does not parse and a document that does not deserialize may be a user's typing or the program's own constant (`FormatException`, `SerializationException`); an argument, the state of an object the program holds and the program's arithmetic are the program's (`ArgumentException`, `InvalidOperationException`, `KeyNotFoundException`, `OverflowException`). The marker is a default for handlers and never a disposition. Whether a program carries on, reports, retries or stops when a failure reaches it is the handler's decision — a missing file the user chose and a missing database file the program was installed with raise the same class, and only the program knows which one it can do without — and an unmarked exception that a handler expects, a `FormatException` from text a user typed among them, is handled by naming its class. The failure types this document declares are `IOException` and the classes deriving from it ([§7.5](#sec-io-exception-types)), `TimeoutException` ([§6.1](#sec-cancellationtoken-and-cancellationtokensource)), `ChannelClosedException` and `ChannelFullException` ([§6.9](#sec-channel)), and `DomainFailedException` ([§6.8](#sec-domain)), which carries a failure raised in another domain across the join; no built-in class is one. Bands 2 and 3 have value shapes as their mechanism; where a member also offers a throwing tier (the no-dual rule, below), that tier's class takes the marker its origin gives: end of input at `Console.ReadKey` is the environment's, and `EndOfStreamException` is a failure. Whether a class this document declares implements the marker changes only by recorded ruling, as the taxonomy grows only by one (language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)); a class a later revision adds states its marker by the same test, the network family among them.
````

**L5** — `Bezel_Standard_Library.md`, §1, the no-dual rule.

Old:

````text
A stance is the caller's. It does not make the exception a failure or a defect, which is the condition's (*Failures and defects*, above): the throwing `Parse` raises a failure and the throwing indexer a defect.
````

New:

````text
A stance is the caller's. It does not mark the exception, and the marker does not choose the stance (*The failure marker*, above): the throwing `Parse` raises `FormatException` whoever supplied the text.
````

**L6** — `Bezel_Standard_Library.md`, §2.3, the `To*` contract.

Old:

````text
and `ParseRangeException` ([§9.1](#sec-integer-types); an `OverflowException`) when the value does not fit
````

New:

````text
and `OverflowException` when the value does not fit
````

**L7** — `Bezel_Standard_Library.md`, §2.5 *Parse and range contracts*.

Old:

````text
a band-1 defect of the arithmetic domain ([§1](#sec-failure-signal-rule))
````

New:

````text
a band-1 condition of the arithmetic domain ([§1](#sec-failure-signal-rule))
````

**L8** — `Bezel_Standard_Library.md`, §2.6, the `ToString(format)` sentence.

Old:

````text
`ToString(format)` raises `FormatException` for a specifier outside its vocabulary. The specifier is the program's argument, so the condition is a band-1 defect raised with a failure class. It is the recorded misfit that [§2.18](#sec-iformattable) states under [§1](#sec-failure-signal-rule)'s rule, and it stands until its own ruling.
````

New:

````text
`ToString(format)` raises `FormatException` for a specifier outside its vocabulary ([§2.18](#sec-iformattable)).
````

**L9** — `Bezel_Standard_Library.md`, §2.14, the registry paragraph.

Old:

````text
 (The write-side half is a band-1 defect raised with a failure class, a recorded misfit under [§1](#sec-failure-signal-rule)'s rule, as a `Weak` reference to a target never strongly reached is.)
````

New: *(deleted)*

**L10** — `Bezel_Standard_Library.md`, §2.15, the `SerializationException` declaration.

Old:

````text
public class SerializationException: Exception, IFailure
````

New:

````text
public class SerializationException: Exception
````

**L11** — `Bezel_Standard_Library.md`, §2.18, the specifier vocabulary.

Old:

````text
 The condition is a band-1 defect raised with a failure class: a recorded misfit under [§1](#sec-failure-signal-rule)'s rule, shared by `Guid.ToString(format)` ([§2.6](#sec-guid)) and `StringBuilder.AppendFormat` ([§3.1](#sec-stringbuilder)), which stands until its own ruling.
````

New: *(deleted)*

**L12** — `Bezel_Standard_Library.md`, §3.1, *Contracts*.

Old:

````text
 The format string is the program's argument, so each of these conditions is a band-1 defect raised with a failure class. They are recorded misfits under [§1](#sec-failure-signal-rule)'s rule, with the specifier of [§2.18](#sec-iformattable), and they stand until their own ruling.
````

New: *(deleted)*

**L13** — `Bezel_Standard_Library.md`, §3.2, *Semantics of the standard encodings*.

Old:

````text
 (The `GetBytes` half is a band-1 defect raised with a failure class, a recorded misfit under [§1](#sec-failure-signal-rule)'s rule: the program chose an encoding that cannot represent its own text.)
````

New: *(deleted)*

**L14** — `Bezel_Standard_Library.md`, §6.8, after the declarations.

Old:

````text
The exception's kind crosses with them (language spec [§30.14](Bezel_Language_Spec.md#sec-multiple-domains)): the join raises `DomainFailedException` where the original exception's class implements `IFailure`, and `DomainFaultedException` itself for a defect and for cancellation.
````

New:

````text
The failure marker crosses with them (language spec [§30.14](Bezel_Language_Spec.md#sec-multiple-domains)): the join raises `DomainFailedException` where the original exception's class implements `IFailure`, and `DomainFaultedException` itself for every other exception, cancellation included.
````

**L15** — `Bezel_Standard_Library.md`, §9.1, *The radix alphabet*.

Old:

````text
a value outside the type's range raising `ParseRangeException` (*The range failure*, below)
````

New:

````text
a value outside the type's range raising `OverflowException`
````

**L16** — `Bezel_Standard_Library.md`, §9.1, *The range failure* (removed with its declaration).

Old:

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

New: *(deleted)*

**L17** — `Bezel_Standard_Library.md`, §9.2, *Input grammar*.

Old:

````text
not the `ParseRangeException` below
````

New:

````text
not the `OverflowException` below
````

**L18** — `Bezel_Standard_Library.md`, §9.2, *Parsing and range*.

Old:

````text
raises `ParseRangeException` ([§9.1](#sec-integer-types)): the type cannot represent the number the input spells
````

New:

````text
raises `OverflowException`: the type cannot represent the number the input spells
````

**L19** — `Bezel_Standard_Library.md`, §9.3, the range sentence.

Old:

````text
raises — raises `ParseRangeException` ([§9.1](#sec-integer-types)).
````

New:

````text
raises — raises `OverflowException`.
````

**R1** — `Bezel_Design_Rationale.md`, §11.4, the closing sentence.

Old:

````text
The line between the rule's first and fourth bands is visible in the exception's class ([§11.12](#sec-failures-and-defects)).
````

New:

````text
Where a band-4 condition comes from the environment or a peer, the exception's class says so ([§11.12](#sec-failures-and-defects)); no class says what a handler does with it.
````

**R2** — `Bezel_Design_Rationale.md`, §11.12, the heading and the first paragraph.

Old:

````text
### 11.12 Failures and defects: the kind is a marker interface on the class

(Proposal 0211.) The failure-signal rule (§11.4) separates a caller's bug from a failure consumed at a boundary. The difference is a fact about the exception's class — it implements `IFailure` or it does not (spec §31.4) — so that a handler can name it: the boundary arm names the marker, where an arm on `Exception` takes both.
````

New:

````text
### 11.12 The failure marker: origin on the class, disposition in the handler

(Proposals 0211 and 0214.) A handler at a boundary often wants to take what the outside world did to the program — a missing file, a dropped connection, a closed channel, a deadline — and let everything else through. The marker makes that a fact about the exception's class: it implements `IFailure` or it does not (spec §31.4), and the boundary arm names the marker where an arm on `Exception` takes everything.

**Origin, not disposition.** The class states one thing its raiser knows: whether the environment or a peer produced the condition. It does not state whether the condition is expected, and a class without the marker does not state that the program is wrong. A reading that named the unmarked side *defect* and the marked side "a condition a correct program can meet" was declined (proposal 0214), because a measured sample of 272 task–operation pairs (`docs/research/failure-kind-by-context/`, re-rated blind on 100 of them) refutes both halves of it. The environment and peer classes were a bug in 3 of 106 rows. The classes it called defects were a bug in only about half of theirs. And what a program does about a condition varies with the call far more than with the class: the best rule keyed on the type predicts 53% of the dispositions, who supplied the operand 70%. Text that does not parse and a document that does not deserialize sit between: whether the input was a user's or the program's own constant decides, and the library cannot see it (bugs in 8 of their 66 rows, most of the bugs raised with a marked class in that reading). So `FormatException` and `SerializationException` carry no marker, and a handler that expects bad input names them. The disposition — carry on, report, retry, stop — is always the handler's: catching a missing database file and stopping is a handler.
````

**R3** — `Bezel_Design_Rationale.md`, §11.12 *An interface, not a base class*, the first ground.

Old:

````text
A class has one base, so a band drawn as a subtree cannot hold a class that must also stay under another: the out-of-range `Parse` input, which existing `OverflowException` arms take, and the fault a domain join raises, which existing `DomainFaultedException` arms take. Each would have had to leave its base, and the arms written against that base would have stopped matching.
````

New:

````text
A class has one base, so a band drawn as a subtree cannot hold a class that must also stay under another: the fault a domain join raises for a failure, which existing `DomainFaultedException` arms take, and a user's class marked under a base of the documents (`class StaleOrder: InvalidOperationException, IFailure`), which the user's arms on that base take. Each would have had to leave its base, and the arms written against that base would have stopped matching.
````

**R4** — `Bezel_Design_Rationale.md`, §11.12 *An interface, not a base class*, the third ground.

Old:

````text
Third, the precedent is against the subtree: Java drew its checked/unchecked line by base class and filed `NumberFormatException` — malformed input, the plainest failure there is — under the unchecked `IllegalArgumentException`, because its is-a parent was on the other side of the line. A kind that cuts across the is-a tree needs a mechanism that can.
````

New:

````text
Third, the precedent is against the subtree: Java drew its checked/unchecked line by base class, and its own library added `UncheckedIOException` to carry I/O errors across it where a use needed them on the other side. A marker that cuts across the is-a tree needs a mechanism that can.
````

**R5** — `Bezel_Design_Rationale.md`, §11.12 *Why the marker has no members, and who may carry it*, the second restriction.

Old:

````text
A second restriction keeps the documents' own classification true: a class under a built-in or standard-library class that is not a failure may not list the marker. Without it `class C: OperationCanceledException, IFailure` would make a failure arm take a cancellation, and a user's failure under `InvalidOperationException` would be taken by every state-defect arm — the swallowing stdlib §4.8 keeps `KeyNotFoundException` clear of. The library's two bridging classes are the only ones across that line, each recorded with its reason; a user hierarchy under a user base stays free to mix kinds, since no arm on a class of the documents is affected.
````

New:

````text
A second restriction keeps cancellation apart: a class under `OperationCanceledException` may not list the marker. Cancellation answers a request the program itself made, and a failure arm — a retry loop, `IFailure => ()` — that took it would retry or swallow the request to stop. The broader restriction, no marker under any unmarked class of the documents, was declined (proposal 0214): an unmarked class makes no statement, so a user class `StaleOrder: InvalidOperationException, IFailure` contradicts nothing, and an arm on the base taking it is what the user's own derivation asked for. The swallowing stdlib §4.8 guards against is the library choosing a base for its own classes, which the library still controls.
````

**R6** — `Bezel_Design_Rationale.md`, §11.12 *Unmarked means defect*.

Old:

````text
**Unmarked means defect.** A user exception class is a defect report until it lists the marker. The other default would let a boundary arm swallow whatever a library author forgot to classify, which is the failure this entry exists to remove; the cost of the chosen default is one interface name on each class meant to be handled.
````

New:

````text
**Unmarked makes no statement.** A class that does not list the marker says nothing about where its condition came from. Reading it as a bug report was declined (proposal 0214): about half of the sample's rows raised with such a class were not bugs. Reading it as a failure was declined too (proposal 0211): an `IFailure` arm would then take whatever a library author forgot to classify, which is what the marker exists to keep out. The cost of the chosen default is one interface name on each class meant to be taken by an `IFailure` arm.
````

**R7** — `Bezel_Design_Rationale.md`, §11.12 *What the marker does not do*.

Old:

````text
Abandoning the process or the domain on a defect was weighed and declined: it would skip the cleanup the exception edge guarantees, and it would make a contract violation uncatchable, where spec §22.16 makes it a catchable exception and the test oracle and the supervisor both depend on catching one. It leaves the query/assert tiers alone: a missing key and an empty collection stay defects in their throwing forms, because the program owns the state each tests (stdlib §1, *Failures and defects*); the throwing `Parse` raises a failure, because the text is the outside's. The kind is the condition's, not the tier's.
````

New:

````text
Abandoning the process or the domain on a bug was weighed and declined: it would skip the cleanup the exception edge guarantees, and it would make a contract violation uncatchable, where spec §22.16 makes it a catchable exception and the test oracle and the supervisor both depend on catching one. It leaves the query/assert tiers alone: a throwing form raises its condition's class whatever the caller's stance — the missing key and the empty collection their unmarked classes, `Parse` its unmarked `FormatException`, `Console.ReadKey` its marked `EndOfStreamException` (stdlib §1, *The failure marker*). The marker is the condition's, not the tier's. And it does not convert: a caller for whom a failure at one call means a bug — a schema file shipped with the service — has no one-token form to say so. That conversion is deferred (proposal 0214) until programs written against the marker show it is needed.
````

**R8** — `Bezel_Design_Rationale.md`, §11.12 *Reopening bars*.

Old:

````text
For the base class: evidence that the catch-arm binding rule misleads readers, together with a migration for the two bridging classes.
````

New:

````text
For the base class: evidence that the catch-arm binding rule misleads readers, together with a migration for the domain join's bridging class and for user classes marked under a base of the documents. For marking a data class (`FormatException`, `SerializationException`) again: programs written against the marker whose boundaries answer bad input through an `IFailure` arm far more often than they name the class, with the bugs such an arm would then answer as routine counted.
````

**G2-1** — `Bezel_Guide_02_Errors_and_Nullability.md`, the pin.

Old:

````text
re-validated against v0.356.*
````

New:

````text
re-validated against v0.358.*
````

**G2-2** — `Bezel_Guide_02_Errors_and_Nullability.md`, the opening list.

Old:

````text
- A **caller bug** is a contract (`requires`, §7) or a defect exception: a report, not something to handle.
- A **failure** — I/O, network, bad input: anything a correct program can meet, logged or retried rather than branched on — is a typed exception whose class implements `IFailure` (§4).
````

New:

````text
- A **caller bug** is a contract (`requires`, §7) or one of the residual exceptions (`ArgumentException`, `InvalidOperationException`): a report to the calling code.
- A **failure consumed at a boundary** — logged, retried or reported rather than branched on — is a typed exception. When the environment or a peer produced it (I/O, a closed channel, a deadline), its class implements `IFailure` (§4).
````

**G2-3** — `Bezel_Guide_02_Errors_and_Nullability.md`, after the opening list.

Old:

````text
exceptions are the boundary-and-defect tool, not the default.
````

New:

````text
exceptions are the boundary tool, not the default.
````

**G2-3b** — `Bezel_Guide_02_Errors_and_Nullability.md`, §2, the first sentence.

Old:

````text
Expected failure is a value, not a throw — exceptions are the boundary-and-defect tool (§4).
````

New:

````text
Expected failure is a value, not a throw — exceptions are the boundary tool (§4).
````

**G2-4** — `Bezel_Guide_02_Errors_and_Nullability.md`, §4, the first sentence.

Old:

````text
the failure consumed at a boundary, and the defect a `requires` contract (§7) cannot state
````

New:

````text
the failure consumed at a boundary, and the bug a `requires` contract (§7) cannot state
````

**G2-5** — `Bezel_Guide_02_Errors_and_Nullability.md`, §4, *Failures and defects* through the class-of-your-own paragraph.

Old:

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

A class may list `IFailure` only if nothing above it but `Exception` is a built-in or standard-library class (spec §31.4). So a failure class that derives from `InvalidOperationException` or `ArgumentException` — a habit carried over from C# — has to be re-based before it can be marked: `class OrderError(message: string): Exception(message), IFailure`. Arms that named the old base (`InvalidOperationException => …`) no longer take it, so find those arms first; an arm on the class itself, or on `IFailure`, is unaffected. A class that derives from a failure type (`IOException`, `FormatException`) is a failure already and needs no change.

Two habits keep the kinds honest. Wrap a failure in a failure and never wrap a defect: re-raise it. And when out-of-range text matters, catch `FormatException` and `ParseRangeException` (or just `IFailure`); `ParseRangeException` is the failure `int.Parse("99999999999")` raises, distinct from the arithmetic `OverflowException` of `a * b`, which is a defect.
````

New:

````text
**The failure marker.** Some exceptions say where they came from. A class that implements the marker interface `IFailure` is a *failure*: the outside world disagreed — a missing file, a refused permission, a peer that closed its channel, a deadline. The `IOException` family, `TimeoutException`, `ChannelClosedException`, `ChannelFullException` and `DomainFailedException` are failures (spec §31.4, stdlib §1). Every other class makes no statement: a `FormatException` may be a user's typo or a bug in your own constant, a `KeyNotFoundException` a bad key from a request or a slip in your own bookkeeping. (`OperationCanceledException` is apart: it answers a cancellation you asked for.)

Keep three things apart: *the class says who could have caused it; the call says whether you expected it; the handler says what happens next.* The marker is the first, and only the first. A missing file is a failure whether the user picked the file or your program cannot start without it; what you do about it is your handler's choice.

That gives a boundary its arms. Where you would have reached for `Exception`, name what this boundary expects — the classes it can answer, and the marker for what the environment and its peers do:

```bezel
method Serve(request: Request): Response
  try
    return Dispatch(request);
  catch
    var e: FormatException => do             // bad client input: expected here
      return Response.BadRequest(e.Message);
    end,
    var f: IFailure => do                    // the environment or an upstream peer
      return Response.BadGateway(f.Message);
    end,
    var ex: Exception => do                  // everything else: reported, not hidden
      ReportBug(ex);
      return Response.InternalError();
    end
  end;
end Serve;
```

The `FormatException` arm is this service's statement that malformed input from a client is expected here; nothing in the class says so, because the same class reports a malformed constant in your own code. The `IFailure` arm takes what the environment or an upstream peer did; `f` is an ordinary `Exception` inside the arm. The last arm is there only because this boundary is a supervisor that must keep serving. Most boundaries have no last arm: anything else then propagates, and an uncaught exception ends the program with its report (spec §33.3). Where cancellation can reach a supervisor, give it its own arm ahead of the `Exception` arm — `OperationCanceledException => throw` — so that it is not reported as a bug. One arm on bare `Exception` that answers everything the same way is the habit to drop: it answers a bug as if it were bad input.

The marker is a default, not a verdict. If the service cannot run without a file, an `IFailure` arm above a lazy open of it answers its absence as routine. Open such a file at start-up, where no arm is above it, or catch `FileNotFoundException` there and stop.

An exception class of your own makes no statement until you say otherwise:

```bezel
public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;
```

A class may list `IFailure` under any base except `OperationCanceledException` (spec §31.4). A hierarchy carried over from C# keeps its base: `class StaleOrder(message: string): InvalidOperationException(message), IFailure` is taken both by an `IFailure` arm and by an arm on `InvalidOperationException`. A class that derives from a failure type (`IOException`) is a failure already and needs no listing. Mark a class when its condition is the outside world's, or when your application wants its boundary's `IFailure` arm to take it — a `ConfigError` for a configuration file the operator wrote, say.

Two habits keep the marker honest. When you wrap, the wrapper's class is what the handler above will read: wrap a failure in a failure, and re-raise an exception you cannot vouch for rather than wrapping it in a class that lists `IFailure`. And where bad text is expected, name what `int.Parse` raises — `FormatException` for a malformed numeral, `OverflowException` for an out-of-range one — or, better, take `ParseOrNull` and decide at the call (§2).
````

**G2-6** — `Bezel_Guide_02_Errors_and_Nullability.md`, §5, the `Handle` example's comment.

Old:

````text
    var ex: IFailure => RecordError(ex)   // ex is an Exception here; a bug propagates
````

New:

````text
    var ex: IFailure => RecordError(ex)   // ex is an Exception here; anything else propagates
````

**G2-7** — `Bezel_Guide_02_Errors_and_Nullability.md`, §7, the boundary-consumed row.

Old:

````text
| a typed exception whose class implements `IFailure`, producer-local (spec §31.4); catch it at the boundary with an `IFailure` arm and copy out what you keep (§4, §5) |
````

New:

````text
| a typed exception, producer-local (spec §31.4), listing `IFailure` when the environment or a peer produced it; catch it at the boundary — by name, or with an `IFailure` arm — and copy out what you keep (§4, §5) |
````

**G2-8** — `Bezel_Guide_02_Errors_and_Nullability.md`, §7, the boundary row.

Old:

````text
| Handling whatever goes wrong at a boundary | an `IFailure` arm; a separate `Exception` arm only in a supervisor that reports the bug, never one arm for both (§4) |
````

New:

````text
| Handling what goes wrong at a boundary | an arm for each class this boundary expects (`FormatException` for bad client input) and an `IFailure` arm for what the environment and peers do; a separate `Exception` arm only in a supervisor that reports the rest, never one arm for everything (§4) |
````

**G2-9** — `Bezel_Guide_02_Errors_and_Nullability.md`, §7, the closing paragraph.

Old:

````text
And at the boundary the arm names `IFailure`: an arm on bare `Exception` takes the bugs too.
````

New:

````text
And at the boundary an arm names what the boundary expects — the classes it can answer, and `IFailure` for the outside world's — where an arm on bare `Exception` takes the bugs too.
````

**G4-1** — `Bezel_Guide_04_Concurrency.md`, §5, the join.

Old:

````text
Its kind does cross: a failure in the child is joined as `DomainFailedException`, a `DomainFaultedException` that an `IFailure` arm also takes, and a bug in the child is joined as the plain class, which that arm lets pass (Guide 2 §4).
````

New:

````text
The failure marker does cross: a failure in the child is joined as `DomainFailedException`, a `DomainFaultedException` that an `IFailure` arm also takes, and any other fault in the child is joined as the plain class, which that arm lets pass (Guide 2 §4).
````

**G4-2** — `Bezel_Guide_04_Concurrency.md`, §8, the mutable-static bullet (re-trace: §22.12 form (iv) was retired at v0.334).

Old:

````text
- **A mutable static** is a compile error unless it is one of four admitted forms:
````

New:

````text
- **A mutable static** is a compile error unless it is one of three admitted forms:
````

**G4-3** — `Bezel_Guide_04_Concurrency.md`, §8, the same bullet, form (iii) and the retired form (iv).

Old:

````text
admitted *with a warning* and no cross-domain promises; (iv) the owning-inline-array slot pool, likewise warned.
````

New:

````text
admitted *with a warning* and no cross-domain promises. (An owning-inline-array slot pool is a `static readonly` field instead: warned, its slots fixed, disposed at exit.)
````

**G6-1** — `Bezel_Guide_06_Contracts_and_Purity.md`, §2, the first paragraph.

Old:

````text
with defect exceptions like `ArgumentException` as the pre-contract residual
````

New:

````text
with residual exceptions like `ArgumentException` at a pre-contract boundary
````

**G6-2** — `Bezel_Guide_06_Contracts_and_Purity.md`, §2, the supervision-boundary paragraph.

Old:

````text
the sanctioned signal is a typed exception whose class implements `IFailure`: a failure (Guide 2 §4; the failure-signal rule's band 4, stdlib §1).
````

New:

````text
the sanctioned signal is a typed exception (Guide 2 §4; the failure-signal rule's band 4, stdlib §1).
````

**G6-3** — `Bezel_Guide_06_Contracts_and_Purity.md`, §2, the `Listen` rewrite's comment.

Old:

````text
    // a requires. FormatException is the built-in input failure: its class
    // implements IFailure (spec §31.4).
````

New:

````text
    // a requires. FormatException is the built-in format fault (spec §31.4).
````

**G6-4** — `Bezel_Guide_06_Contracts_and_Purity.md`, §2, why not the throwing `int.Parse`.

Old:

````text
You may. What it raises is a failure either way — `FormatException` for a malformed numeral, `ParseRangeException` for an out-of-range one (stdlib §9.1, §2.3) — because the text came from outside the program, and a boundary's `IFailure` arm takes both (Guide 2 §4). The explicit throw is for the wording: `int.Parse` reports two conditions in the numeral's terms, while `Listen`'s condition is single — *not a port*. The explicit throw keeps one condition on one signal, in the domain's own words. The kind is also what rules out `ArgumentException`: the `Argument*` family is part of band 1's pre-contract defect residual (stdlib §1) — the exception that blames the *calling code* — and no caller is at fault here. Throwing it would plant a defect report in the boundary's log — telling whoever triages that log that the calling code needs fixing, when it does not; `FormatException` blames the input's shape, which is what actually failed.
````

New:

````text
You may, and the boundary sees an exception either way. But `int.Parse` splits the condition: it raises `FormatException` for a malformed numeral and `OverflowException` for an out-of-range one (stdlib §9.1, §2.3), while `Listen`'s condition is single — *not a port*. The explicit throw keeps one condition on one signal, in the domain's own words, so a boundary that expects bad ports names one class. What rules out `ArgumentException` is what it says: the `Argument*` family is band 1's pre-contract residual (stdlib §1) — the exception that blames the *calling code* — and no caller is at fault here. Throwing it would plant a report in the boundary's log telling whoever triages it that the calling code needs fixing, when it does not; `FormatException` blames the input's shape, which is what actually failed. Neither class implements `IFailure`: whether bad text is expected depends on who supplied it, which only the boundary knows (spec §31.4), so the boundary that expects it names `FormatException`.
````

**G6-5** — `Bezel_Guide_06_Contracts_and_Purity.md`, §2, the two-question test.

Old:

````text
The kinds follow the answer: a broken `requires` raises `ContractViolationException`, a defect report that a boundary's `IFailure` arm lets pass, and the typed exception for bad input is a failure, which that arm takes (spec §31.4).
````

New:

````text
The exceptions follow the answer: a broken `requires` raises `ContractViolationException`, which reports the calling code, and the typed exception for bad input blames the input; the boundary names the one it expects (Guide 2 §4).
````

**T1** — `Bezel_Tour.md`, §6, exceptions.

Old:

````text
**Exceptions** are for what the caller does not branch on, and are of two kinds: a *failure* (a missing file, bad input), whose class implements `IFailure`, and a *defect* report, which is a bug. `IFailure => …` is the arm that takes every failure and no bug.
````

New:

````text
**Exceptions** are for what the caller does not branch on. A class that implements `IFailure` is a *failure*: the outside world disagreed (a missing file, a closed channel), and `IFailure => …` is the arm that takes all of them. What to do about one is the handler's call.
````

**C1** — `Bezel_For_Code_Generation.md`, §5.1, the three access tiers.

Old:

````text
(the call asserts the value is there and raises otherwise: a *failure* where the condition is the outside's — `Parse`, `Console.ReadKey` — and a *defect* where it is the program's own state — `dict[key]`, `First()`, `PopAt`, `Pop`, `Dequeue`)
````

New:

````text
(the call asserts the value is there and raises otherwise — `Parse`, `Console.ReadKey`, `dict[key]`, `First()`, `PopAt`, `Pop`, `Dequeue`; of these only `Console.ReadKey`'s `EndOfStreamException` implements `IFailure`)
````

**C2** — `Bezel_For_Code_Generation.md`, §5.2, the `Convert` comment.

Old:

````text
end                                                    // failures: FormatException, ParseRangeException
````

New:

````text
end                                                    // raises FormatException, OverflowException
````

**C3** — `Bezel_For_Code_Generation.md`, §5.2, the exception hierarchy.

Old:

````text
**Kinds (§31.4):** a class that implements the member-less interface `IFailure` is a *failure* — `FormatException`, the `IOException` family, `SerializationException`, `TimeoutException`, `ChannelClosedException`, `ChannelFullException`, `ParseRangeException` (an `OverflowException`: out-of-range `Parse`/`Convert.To*` input) and `DomainFailedException` (a `DomainFaultedException`: a failure joined from another domain); every other is a *defect* report; `OperationCanceledException` is neither. Only an `Exception`-derived class may list `IFailure`, and not one under a built-in or stdlib class that is not itself a failure.
````

New:

````text
**The failure marker (§31.4):** a class that implements the member-less interface `IFailure` is a *failure* — the environment or a peer produced it: the `IOException` family, `TimeoutException`, `ChannelClosedException`, `ChannelFullException` and `DomainFailedException` (a `DomainFaultedException`: a failure joined from another domain). No built-in class is one, and every other class makes no statement (`FormatException`, `SerializationException` and `OverflowException` included); `OperationCanceledException` is apart. Only an `Exception`-derived class may list `IFailure`, and none under `OperationCanceledException`.
````

**C4** — `Bezel_For_Code_Generation.md`, §5.9, `double.Parse` out of range.

Old:

````text
it rounds to nearest-even like a literal and raises `ParseRangeException` exactly where
````

New:

````text
it rounds to nearest-even like a literal and raises `OverflowException` exactly where
````

**C4b** — `Bezel_For_Code_Generation.md`, §5.9, the integer input grammar.

Old:

````text
out of range `ParseRangeException` (an `OverflowException`; a failure)
````

New:

````text
out of range `OverflowException`
````

**C5** — `Bezel_For_Code_Generation.md`, §6, declaring an exception type.

Old:

````text
list `IFailure` when the condition comes from outside the program, since an unmarked class is a defect report (§31.4) — only where nothing in the base chain but `Exception` is a built-in or stdlib class, so a failure class derives from `Exception` or from a user class deriving from it (or from a failure type such as `IOException`, which needs no listing);
````

New:

````text
list `IFailure` when the environment or a peer produces the condition, or when the application's boundary should take it as one — under any base but `OperationCanceledException`, and a class deriving from a failure type such as `IOException` needs no listing; an unmarked class makes no statement (§31.4);
````

**C6** — `Bezel_For_Code_Generation.md`, §6, catching at a boundary.

Old:

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

Leave the second arm out unless the code is a supervisor: a bug should propagate. `IFailure => ()` suppresses failures only.
````

New:

````text
**Catching at a boundary.** Name what the boundary expects, not `Exception`: the classes it can answer by name, and `IFailure` for what the environment and peers raise; an `IFailure` binding is an ordinary `Exception` (§31.2.1).

```bezel
try
  Serve(request);
catch
  var e: FormatException => Respond(400, e.Message),           // bad client input, expected here
  var f: IFailure => Respond(502, f.Message),                  // the environment or an upstream peer
  var ex: Exception => do ReportBug(ex); Respond(500); end     // a supervisor only: the rest reported
end;
```

Leave the last arm out unless the code is a supervisor: a bug should propagate. `IFailure => ()` suppresses failures only; `FormatException` and `SerializationException` are not failures, so a boundary that expects bad input names them.
````

**P1** — `Bezel_Language_Spec.md`, the version header.

Old:

````text
**Version 0.357 — November 2026**
````

New:

````text
**Version 0.358 — November 2026**
````

**P2** — `Bezel_Standard_Library.md`, the companion header.

Old:

````text
**Companion to the Bezel Language Specification, v0.357 — November 2026**
````

New:

````text
**Companion to the Bezel Language Specification, v0.358 — November 2026**
````

**P3** — `Bezel_Design_Rationale.md`, the companion header.

Old:

````text
**Companion to the Bezel Language Specification, v0.357 — November 2026**
````

New:

````text
**Companion to the Bezel Language Specification, v0.358 — November 2026**
````

**P4** — `Bezel_Changelog.md`, the companion header.

Old:

````text
**Companion to the Bezel Language Specification, v0.357 — November 2026**
````

New:

````text
**Companion to the Bezel Language Specification, v0.358 — November 2026**
````

**P5** — `Bezel_For_Code_Generation.md`, the pin.

Old:

````text
*Validated against v0.357.
````

New:

````text
*Validated against v0.358.
````

**P6** — `Bezel_For_Code_Generation.md`, the citation pin.

Old:

````text
are to spec v0.357 and `Bezel_Standard_Library.md` v0.357
````

New:

````text
are to spec v0.358 and `Bezel_Standard_Library.md` v0.358
````

**P7** — `Bezel_For_Code_Generation.md`, the stdlib transcription pin.

Old:

````text
transcribed from `Bezel_Standard_Library.md` v0.357
````

New:

````text
transcribed from `Bezel_Standard_Library.md` v0.358
````

**P8** — `Bezel_Guide_06_Contracts_and_Purity.md`, the pin.

Old:

````text
re-validated against v0.357.*
````

New:

````text
re-validated against v0.358.*
````

**P9** — `Bezel_Tour.md`, the pin.

Old:

````text
*Validated against v0.357.
````

New:

````text
*Validated against v0.358.
````

**P10** — `Bezel_Guide_04_Concurrency.md`, the pin.

Old:

````text
re-validated against v0.331.*
````

New:

````text
re-validated against v0.358.*
````


## 10. Owner questions

The decision of 2026-11-11 settles the design. Three points are left open by it, each with a recommendation; none blocks the landing.

**Q1 — Stdlib §2.5's band label for the `DateTime` range fault.** v0.356 moved it from band 4 to band 1 so that it would not be marked. Under v0.358 band 4 no longer implies the marker, so the move's reason is gone, but moving it back is a ruling about arithmetic over data, which is the checked-arithmetic follow-up's subject. **Recommendation:** keep band 1 now (only the word *defect* changes), and decide it in the checked-arithmetic proposal (§7.2).

**Q2 — `ChannelClosedException` raised for a channel the same code closed** (the note's T33c, a bug). The decision keeps the channel classes marked, and the class cannot see who closed the channel. **Recommendation:** keep it marked; the sender that closes its own channel and then sends is the bug, and an `IFailure` arm around that send is the program's to remove. Revisit only with the §7.1 conversion's evidence.

**Q3 — Data classes under `IOException` in unlanded proposals.** Proposal 0069's `InvalidDataException` and `DecompressionLimitException` derive from `IOException`, so they would be marked by inheritance, though their condition is the input data's — the reason `SerializationException` lost the marker. **Recommendation:** decide at 0069's graduation, by the test of stdlib §1 *The failure marker*: a decompression limit is a capacity limit (marked, like `ChannelFullException`), and corrupt compressed data is data whose supplier the library cannot see (unmarked, so re-based under `Exception` or a data class). Nothing changes here.

## 11. Classifications (process §9)

- **Change class:** design change; it reopens recorded rulings (§5) and is landed under the owner's acceptance of 2026-11-11.
- **Kernel classification (§28/§30.5):** none touched. No K rule, axiom or register row changes; §30.14's join mechanism is unchanged.
- **Body-locality (proposal 0105 §1):** the one remaining conformance restriction is **local** — it reads the declaration's own base chain. No rule is added.
- **Absence check (process §5 item 3b):** before retiring `ParseRangeException` and removing two conformances, `src/` and `tests/` outside the rule corpus were grepped for the three names: nothing names them, and r40 raises `OverflowException` for out-of-range `Parse` input (tracker #8417's probe). No impl-ahead-of-spec delta arises.
- **Composition:** one train. The edits were dry-run as one sequence against v0.357 (§9) and against each other; the composition with any sibling train of the run is the coordinator's gate.

## 12. Graduation record

Graduated as **v0.358** on branch `spec/narrow-failure-2026-11` (from `origin/main` `e49c32aca`, with the research note merged), pending the meaning gate. The edits of §9 applied unchanged in their anchors. Proposal 0211's status row records that its taxonomy was narrowed here. `DECLINED.md` gains ten rows — seven `declined-at-proposal`, two `deferred` (the caller-side conversion; checked arithmetic), one `retired` (`ParseRangeException`) — and eleven rows are annotated in place (§5's reopened and restated rows, D-0005 and proposal 0213's row). The guides re-traced: Guide 2 (v0.356 → v0.358), Guide 4 (v0.331 → v0.358, in full, #8427), Guide 6 and the Tour (v0.357 → v0.358); the code-generation reference's three pins move to v0.358.
