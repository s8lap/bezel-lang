# One way to handle errors?

*Research lane `research/error-model`, 2026-11-10. Non-normative: nothing in the spec, the
standard library, the proposals, the compiler or the tests changed with it, and no proposal or
issue was filed. Pins: documents at **v0.355**, tree `a30bb11df`, compiler release **r40**
(`bezelc 0.0.0-a30bb11d`). Web access was not used; the prior-art section is written from
memory and says so.*

*Revised once after a non-author review of the first version (`ef4dcf78d`, accepted with
notes). The changes: the recommendation is now staged (§8.2); the band table separates the
exception types used in both bands (§2.3); the ownership-model claims are narrowed to what
the model covers (§5.2); the atom is defined once, with its positions (§8.1); the protection
it gives up is named (§3, §9.1).*

*Evidence labels. **MEASURED**: produced by a script in `data/`, a program in `probes/` or
`model/`, re-runnable. **QUOTED**: text of a repository document or source file, cited.
**HYPOTHESIS**: a judgement or a prediction; not evidence.*

## 1. One page for the owner

**The question.** Bezel signals failure in three tiers (exceptions, the `Try…(out x): bool`
shapes, the `…OrNull` forms) plus `Result<T, E>` in some clusters. Can it have one coherent way
to handle errors that is simpler to learn and pleasant to use, without losing what the current
design protects?

**The answer.** Not one *mechanism*. The languages that began with one grew a second:
exception-only languages added value-returning duals, value-only languages kept a panic (§6).
The designs that would force one on Bezel either give up cleanup on the bug path or make every
caller mark errors it never handles (§4). One *rule* is achievable, smaller than the five-band
rule it replaces, in two halves. The first half costs nothing the design protects and is
recommended now. The second half gives up one recorded protection ("catch at boundaries") and
is recommended only behind an evidence gate. `Result`-first stays declined: none of proposal
0076 §6's four conditions is met.

**The one rule** (two sentences, for the user):

> A bug is a *fault* and an expected problem is a *failure*; both are thrown, both clean up,
> and only failures are meant to be handled. At each call the caller decides what a failure
> means: write nothing to pass it up, `catch` it to handle it, or write `(try call)` to get
> `null` instead (`false`, for an operation that returns nothing); `!` does the reverse for a
> value that must be there.

The first sentence is the half recommended now. The `(try call)` stance in the second is the
deferred half; the other stances exist today.

The API author's side of the same rule: *state each operation once.* A query whose answer may
be "nothing there" returns `T?`. An operation that may be unable to do its job throws a
failure. A broken promise by the caller is a `requires` or a fault.

**Recommended now (stages 0–1, §8).** A marker for the failure band (working name `IFailure`),
implemented by the expected-failure exception types. It gives the taught model its three
kinds (fault, failure, absence) and gives `catch` a boundary arm, `IFailure => …`, that takes
every expected failure and lets every bug through; today's `Exception => …` arm takes both,
and the corpora have 641 of those. It works today as a library convention (probe p16). It
reopens no decline and gives up none of the nine protections of §3.

**Designed here, deferred (stage 2).** A paren atom `(try E)` that turns a failure into
`null`. It is what would let the library state each operation once. It is deferred because
(a) it is an interior catch-and-branch by construction, so it gives up protection 5 of §3,
and it reopens decline D-0005, which proposal 0076 §7 records as "Untouched and *reinforced*
— on both grounds"; (b) the evidence for it is library and test-corpus counts, not observed
programs, the class of evidence the project's gates decline to act on; (c) the ownership
model does not cover its result channel, and r40 leaks at one of the positions it would be
used in (probe p19). The gate is in §8.5.

**Headline numbers** (all MEASURED; scripts in `data/`, programs in `probes/`).

| Fact | Number |
|---|---|
| Standard-library member declarations (v0.355) | 1,342 |
| … that are a `Try` shape / an `…OrNull` form | 94 (54 names) / 26 (11 names): **120, 8.9%** |
| … that return `Result` or another choice (outside `Result`'s own combinators) | **0** |
| One operation spelled in two or three tiers on the same type | 89 families; 14 are three-way |
| Stdlib sections that document an expected failure / of those, with no value-shaped form | 33 of 82 / **15** (all of `System.IO`, serialization, `Encoding`) |
| Corpus programs scanned (rule corpus, doc examples, lit, gtest-embedded) | 13,522 files, about 36,900 programs |
| `throw new` sites / `catch` arms | 1,575 / 3,482 |
| `catch` arms that name a pure bug type (`IndexOutOfRangeException`, `ContractViolationException`, …) | **589 (17%)** |
| `catch` arms that name a type used for bugs *and* for non-bug conditions (`InvalidOperationException` 529, `OverflowException` 434, `ArgumentException` 228) | **1,191 (34%)** |
| `catch` arms that name bare `Exception` (take bugs and failures alike) | 641 (18%) |
| Calls to a `Try` member / an `…OrNull` member / `Result<` mentions | 1,389 / 644 / 350 |
| Cost of one expected failure on r40: `TryParse` vs `Parse` + `catch` | 7.5 ns vs 73 ns (success path: 8 ns both) |
| Exceptions on r40 use unwind tables | no: a pending-exception slot checked after each call |
| Ownership model (pinned at v0.345), existing `try` / empty catch-all fragment: checked / accepted / unsound | 333,648 / 77,559 / **0** |
| Same model, an outer owned local assigned inside the protected block: checked / accepted / unsound | 19,565 / 4,497 / **0** |

The two model rows are about constructs that exist. The model has no calls, values, `null` or
exception types, so it does not model the atom (§5.2).

**What the rule removes.** Now, with the marker alone: nothing from the library; for the user,
the bug-swallowing boundary arm and the five-band classification (three kinds replace it).
With the atom, if it is adopted: the question "which of ten spellings does this operation use,
and which of its three tiers exist?" (the count is in §2.4); behind a warn-first ramp, the
Parse-family duals, 31 declarations as listed (`ParseOrNull` ×10, `ReadKeyOrNull`, `TryParse`
×15, `Convert.TryTo*` ×5; the `Int32` listing stands for eight integer types, stdlib §9.1);
and future duals of operations whose result is not nullable (today 15 sections would each
need new `TryX`/`XOrNull` members, by proposal, to give callers a value-shaped stance). An
operation whose success value may itself be `null` still needs a `Try` shape (a) or a
`catch`. The collection queries (`TryPop`, `TryGetValue`, `FirstOrNull`, …) stay either way:
they are queries, not error handling.

**What it costs.** Now: one built-in interface; one derived class for out-of-range `Parse`
input; a ruling on each place the library names a straddling type (48 type-and-section
pairs, §8.1); Guide 2 and stdlib §1 restated around three kinds. Corpus breakage: **0**.
With the atom: one expression form; protection 5; D-0005 reopened; rationale §11.4 rewritten;
0 corpus sites at introduction and 436 call sites by name when the 31 duals are retired;
about 65 ns more per *failed* `(try int.Parse(s))` than `TryParse` on r40 until the form is
lowered over known members.

**Proposal 0076 §6, the reopening bar for maximal `Result`.** 0 of 4 met (§9.2): (1) no
propagation operator has landed and the `?` collision is unresolved; (2) there is no
single-award async container, and v0.355 instead rejects `Task<Result<resource, E>>`
(probe p07); (3) the governance rule is in stdlib §1 but governs zero payload types; (4) no
real program consumes a decode-cluster `Result` surface (the pilot has not landed and there
are no external users), so whether branch-shaped consumption dominates boundary-shaped
consumption there cannot be observed, and the migration analysis exists only as the site
counts of §2.3. The recommendation does not need any of the four.

## 2. Inventory

### 2.1 The standard library's failure surface

`data/stdlib_inventory.py` reads every fenced `bezel` block of `docs/Bezel_Standard_Library.md`
and records each member declaration (MEASURED; rows in `data/stdlib_members.tsv`).

| Tier | Declarations | Distinct names | Notes |
|---|---|---|---|
| All members | 1,342 | | 1,036 methods, 208 properties, 92 constructors, 6 operators |
| `Try` shape (a) `bool` + nullable `out` | 21 | 9 | `TryGetValue`, `TryPop`, `TryDequeue`, `TryReceive`, `IParsable<T>.TryParse`, … |
| `Try` shape (b) nullable return | 11 | 9 | `TryAcquire`, `TryLock`, `TryUpgrade`, …; the same-stem twin *blocks*, it does not throw |
| `Try` shape (c) `bool` + value `out` | 40 | 26 | the primitive `TryParse` family, `Convert.TryTo*`, span and bit `TryRead*` |
| `Try` shape (d) `bool`, no `out` | 22 | 11 | `TryAdd`, `TrySet*`, span and bit `TryWrite*` |
| `…OrNull` | 26 | 11 | `ParseOrNull`, `GetOrNull`, `FirstOrNull`, `ReadKeyOrNull`, … |
| Returns `Result<…>` or a choice | 3 | | only `Result`'s own `Map`/`MapError`/`AndThen` (and four example methods in §2.20); no producer anywhere |
| Plain nullable return (a primary form) | 24 | | `Console.ReadLine`, `Environment.Get`, `LinkedList<T>.First`, … |

Exceptions. The library names **24 exception types in 173 prose mentions** (MEASURED,
`data/stdlib_exception_mentions.tsv`, after dropping four member names that end in
`Exception`). Only 90 member declarations name their fault on the declaration or its trailing
comment; the rest of the contracts are prose. The document contains **0** `<exception>`
documentation tags (MEASURED), though spec §31.3 calls the tag "the conventional way to
document expected exceptions" (QUOTED). No member's failure contract is machine-readable.

### 2.2 Where the tiers duplicate each other

`data/stdlib_tier_families.tsv` lists every type on which one operation has a plain form and a
`Try` or `…OrNull` form with the same stem (MEASURED): **89 families**.

- 10 offer all three tiers, and all ten are `Parse` (`Parse` / `TryParse` / `ParseOrNull` on
  `Int32`, `Double`, `Float`, `Decimal`, `Boolean`, `Char`, `DateTime`, `DateOnly`, `TimeOnly`,
  `Guid`). By hand, four more are three-way where the script's name matching misses a twin
  (`dict[key]` / `TryGetValue` / `GetOrNull` on the three dictionaries, whose plain form is
  an indexer; `Get` / `TryGetValue` / `GetOrNull` on `OwningSlotMap`): 14.
- 71 offer the plain form and a `Try` form only; 10 of those are the blocking/non-blocking
  pairs of shape (b), which are two operations, not two tiers.
- 8 offer the plain form and an `…OrNull` form only (`First`/`FirstOrNull`, …, `ReadKey`).

So which stance a caller may take depends on the member. There is no `PopOrNull`, no
`TryFirst`, no `Json.TryParse`, no `File.TryReadAllText`. `data/stance_gap.py` (MEASURED):
of 82 library sections, 33 name an expected (non-bug) failure type; 18 of those declare a
`Try` or `…OrNull` member; **15 declare none** — §2.15 serialization formats, §3.2 `Encoding`,
§6.7 task helpers, §6.8 `Domain`, §7.1–§7.5 all of `System.IO`, and others. The library's own
text records this for the decode cluster: "The throwing form is the surface's one spelling (no
value-shaped dual is declared, and no error-payload type)" (QUOTED, stdlib §2.15).

The duals are defined in terms of the throwing form's exceptions. `ParseOrNull`: "null
exactly where Parse(s) raises (the FormatException/OverflowException contract)"; `TryParse`
with a radix: "false (out = 0) exactly where Parse(s, radix) raises" (QUOTED, stdlib §9.1).
A dual is, by its own definition, a mechanical function of the throwing form.

### 2.3 How the corpora use the tiers

`data/corpus_usage.py` scans four corpora with comments and string contents blanked
(MEASURED; `data/corpus_usage.tsv`, `.json`). Doc examples were extracted with
`scripts/extract_doc_examples.py` on an exported copy (802 cases, 688 after dropping `skip`
rows; 67 are chained, so a few sites are counted twice). gtest programs are C++ string
literals; a "program" there is one `module X;` header, and expectation strings in the same
files are scanned with the programs, so the gtest column carries a little noise.

| | rules | doc examples | lit | gtest | total |
|---|---|---|---|---|---|
| files | 5,902 | 688 | 2,574 | 4,358 | 13,522 |
| programs | 5,902 | 688 | 2,574 | ≈27,745 | ≈36,909 |
| `throw new` sites | 383 | 52 | 38 | 1,102 | 1,575 |
| `try` statements | 705 | 21 | 70 | 2,648 | 3,444 |
| `catch` blocks | 689 | 19 | 43 | 2,442 | 3,193 |
| `finally` | 93 | 10 | 32 | 363 | 498 |
| calls to a `Try…` member | 145 | 125 | 71 | 1,048 | 1,389 |
| calls to an `…OrNull` member | 128 | 61 | 3 | 452 | 644 |
| `Result<` mentions | 33 | 17 | 4 | 296 | 350 |
| `?? throw` | 5 | 5 | 0 | 42 | 52 |
| `using var` | 2,651 | 298 | 540 | 18,437 | 21,926 |
| files with any failure signal | 831 | 132 | 147 | 836 | 1,946 |
| files mixing two or more mechanisms | 10 | 13 | 1 | 62 | 86 |

Which exception types (MEASURED; `data/corpus_throw_types.tsv`; the band lists are in the
script). The failure band is exactly the set §8.1 proposes for the marker. A *straddling*
type is one the library raises both for bugs and for conditions a correct program can meet
(§10.2), so an arm that names it cannot be put in a band by its type.

| | pure bug type | straddling type | failure band | cancellation | bare `Exception` | user or other |
|---|---|---|---|---|---|---|
| `throw new` (1,575) | 34 (2%) | 810 (51%) | 27 (2%) | 0 | 225 (14%) | 479 (30%) |
| `catch` arms (3,482) | 589 (17%) | 1,191 (34%) | 453 (13%) | 30 (1%) | 641 (18%) | 578 (17%) |

The straddling arms are `InvalidOperationException` 529, `OverflowException` 434 and
`ArgumentException` 228.

Three readings. First, these are test corpora: at least 589 arms exist to observe a pure bug
type as a test oracle, and some unknown share of the 1,191 straddling arms do the same. A
design in which bugs stop being catchable breaks between 589 and 1,780 arms. Second, a third
of all arms name a type whose band cannot be read from the type, which is the cost of any
type-keyed split (§8.1 item 5). Third, `Result` is rare: 49 files of 13,522 mention `Result<`,
and the library has no producer. The catch-arm matcher is a regular expression with a stated
limit (a `case` arm nested in a catch arm can be miscounted; it lands in "user or other").

Calls per member name (`data/stdlib_tier_shapes.tsv`, MEASURED): shape (a) 818, (b) 112,
(c) 409, (d) 247, `…OrNull` 653 (the name `TryParse` is counted under both (a) and (c)).

### 2.4 What the duplication costs

MEASURED (`data/doc_cost.py`, `data/doc_cost.json`) unless marked.

- **Decision points for an API author.** Guide 2 §7 "Choosing the right tool" has 10 rows;
  7 choose a failure signal, and one of those (the `Try` pattern) has four sanctioned shapes:
  ten spellings for "this can fail". Stdlib §1 states the choice as a five-band rule, first
  match wins.
- **Decision points for a caller.** Up to three forms per operation, and the set differs per
  member (§2.2).
- **Text.** Guide 2 is 4,220 words; 2,028 of them (§2, §3, §7) teach the choice among
  mechanisms. Stdlib §1's Try convention, failure-signal rule, no-dual rule and payload
  governance: 1,540 words. Code-generation reference §5.1: 448. Rationale §11.4: 432.
- **Decisions.** 71 of 353 changelog revisions mention a `Try` member or an `…OrNull` member.
  17 of 203 numbered proposals mention the tiers ten times or more (0072 alone 92 times).
  The decline index has 11 rows whose subject names a `Try`/`…OrNull` member, 41 on
  exceptions, 7 on `Result`.
- **Implementation lag.** On r40 `int.ParseOrNull`, `long.ParseOrNull` and `double.ParseOrNull`
  do not compile ("type 'int' has no static method or property 'ParseOrNull'"), while
  `bool.ParseOrNull` and `Guid.ParseOrNull` do (MEASURED, probe p17). A dual that is
  a function of its twin still has to be built, tested and documented once per type.

## 3. What the current design protects

Any candidate is judged against these. Each is QUOTED from the cited section, v0.355.

1. **Cleanup on every exit edge, with no rule for the programmer.** The `finally`, `using var`
   and owned-local disposals sit on "the single establishment-ordered cleanup stack" (spec
   §31.2); "exception edges discharge implicitly" (spec §35.4, K3.15).
2. **No first-class caught error.** "Exception instances themselves cannot escape the arm that
   caught them" (spec §31.1). This is what keeps one in-flight slot, no exception owning
   another, and later-wins supersession statable (rationale §11.8).
3. **Few throw edges.** Allocation failure terminates rather than throws, because a catchable
   one "would put an exception edge on every `new`" (spec §31.4).
4. **Bugs are reports, not results.** "A defect surface is never a recovery surface" (stdlib
   §1 band 1); contract clauses raise `ContractViolationException` and reject `throw` inside
   them (spec §22.16).
5. **One mechanism per fault condition** (stdlib §1, the no-dual rule), and **catch at
   boundaries**: "`catch` is a boundary-and-supervision tool — convert, log, retry, report,
   re-raise. Branching program logic on caught exception types at an interior call site is
   the signal to check the rule for a value-shaped tier" (Guide 2 §7; proposed by 0076 §6).
   Design F's atom is an interior catch-and-branch, so it gives the second half of this up
   wherever it is used. The marker alone does not.
6. **A governed, small exception taxonomy**, producer-local (spec §31.4).
7. **Domain isolation.** "no exception class is sendable"; a fault crosses as
   `DomainFaultedException` carrying strings (spec §30.14).
8. **Announced control flow** (proposal 0061 §2: an operator that returns from inside an
   expression is "against the announced-flow personality").
9. **No `throws` clause** and nothing forcing callers to handle (spec §31.3).

## 4. Candidate designs

For each: the single thing a user learns, what it removes, what it costs, what it breaks
(MEASURED on the corpora where a count is possible), and the migration path.

### A. Exceptions only, with a blanket call-site `try?`

- **Sketch.** `var n := try? int.Parse(s);` catches every exception from the operand and
  yields `null`.
- **One thing learned.** "Everything throws; `try?` makes it null."
- **Removes.** Every value-tier member with a throwing twin: 104 of the 120 (all but the
  eleven shape-(b) members, whose twin blocks or does not exist; the two `Set` `TryAdd`
  members and `IDeserializer.TryReadNull`, which are primary forms; and the two enum
  `…OrNull` forms, which have no throwing twin).
- **Costs.** It hides bugs. Probe p12 (MEASURED): a blanket form returns `null` for an
  `IndexOutOfRangeException` raised by wrong index arithmetic inside the callee; the form that
  names the expected failure lets it through. It would also swallow cancellation (the Kotlin
  `runCatching` problem, §6). It contradicts §3 item 4, and it is decline D-0005 without an
  answer to its first ground.
- **Breaks.** 0 sites to add; the cost is semantic.
- **Verdict.** Not as stated. Design F is A with the bug band excluded.

### B. Typed throws, errors as values under the hood (Swift 6, P0709)

- **Sketch.** `method Parse(s: string): int throws ParseError;` where `ParseError` is a
  value-like choice; every call to a throwing member is marked `try`; `throw`/`catch` on the
  surface, a value return underneath.
- **One thing learned.** "A function that can fail says so in its type, and you mark the call."
- **Removes.** All 120 tier members, and `Result` as a signalling type.
- **Typing and ownership.** Error payloads must be value-like (spec §35.4 rule 3) so that every
  awaiter of a task can have a copy and a failure can cross a domain; an owned or
  borrow-carrying error payload is excluded by rule. Function types need a failure component,
  and generic higher-order members need to be polymorphic in it; without that a lambda that can
  fail cannot be passed to `Select`. At least 75 stdlib members take a function-typed
  parameter and 49 return a `Task`-family type (MEASURED). `Task<T>` needs a second parameter
  or an erased failure.
- **Costs.** A new dimension in the type system; the largest design in this list. Two of its
  usual benefits are already true of Bezel's implementation: r40 propagates exceptions through
  a slot checked after each call, not through unwind tables, and the throw edge disposes
  exactly what the return edge does (probes p02, p09; §5.1).
- **Breaks.** Every call to a member that can fail needs a mark; the corpora give only a lower
  bound: 1,369 files contain exception syntax, and all 1,575 `throw new` sites and 3,193
  `catch` blocks need a band decision.
- **Verdict.** The right end state for a language starting today; too large for a design
  recorded as mostly settled. Design F leaves the door open (§8.5).

### C. `Result`-first with a propagation operator (Rust, Zig)

- **One thing learned.** "A function returns `Result`; `?` passes the error up."
- **Status.** Recorded as "not currently constructible" (QUOTED, rationale §11.4), with a
  four-condition reopening bar. All four are unmet (§9.2). Nothing measured here changes
  0076's analysis; two facts sharpen it: the library still has 0 `Result` producers, and
  where 0076 found the async container missing (its #2255), the spec now rejects a `Task`
  over an owning choice instead of adding a container (spec §35.4; probe p07).
- **Verdict.** Stays declined (decline-index row *`Result`-first error model*).

### D. Two bands, bugs abandon (Midori)

- **Sketch.** Contract violations, index and null faults, and overflow terminate the process
  (or the domain); only recoverable errors can be handled.
- **One thing learned.** "You cannot catch a bug."
- **Removes.** Catchability of about 15 built-in types; exception edges at every indexing and
  arithmetic site.
- **Costs.** Bezel's cleanup guarantee holds on the bug path today (probe p10: the `using var`
  is disposed before the uncaught report and exit 70). Abandoning a domain without unwinding
  leaves locks held and shared objects half-written; "forcible domain teardown" is already
  rejected on that ground (decline-index row, rationale §12.4). Contracts are welded to a
  catchable `ContractViolationException` (spec §22.16, §31.4).
- **Breaks.** Between 589 `catch` arms (17%, those naming a pure bug type) and 1,780 (51%,
  with the arms naming a straddling type) (MEASURED).
- **Verdict.** Not as stated. Its useful half — bugs are never part of what a caller handles,
  and they keep their own band across every boundary — is adopted in F, with unwinding kept.

### E. D + B (bugs abandon; recoverable errors are typed throws with `try` marking)

The sum of B and D: Midori's actual design. It inherits both costs: B's type-system work and
D's 589 to 1,780 broken arms and lost cleanup. **Verdict:** not recommended; F takes the two-band
model from D and the caller-chosen stance from Midori's `try … else` forms without either
of those costs (the atom has its own: §8.1).

### F. Faults and failures; the caller chooses the stance (§8: the marker now, the atom gated)

- **Sketch.**

  ```bezel
  // proposed: one marker, implemented by the expected-failure exception types
  public interface IFailure end IFailure;          // working name

  var port := (try int.Parse(text)) ?? 8080;       // proposed atom: a failure becomes null
  using var conn := (try new Connection(host));    // an owned result stays owned
  if conn = null then return; end if;
  ```

- **One thing learned.** The rule in §1.
- **Removes.** With the marker: the bug-swallowing boundary arm. With the atom: the band-3 /
  band-4 judgement ("where will this be consumed?") from API design, since the caller
  decides; behind a warn-first ramp, 31 declared duals; and future duals of operations whose
  result is not nullable.
- **Costs.** The marker: one interface, one derived class, a ruling per straddling condition.
  The atom: one expression form, three documents rewritten, one decline reopened, and
  protection 5 of §3 (catch at boundaries) wherever it is used.
- **Breaks.** 0 sites at introduction (MEASURED: no corpus program declares the name
  `IFailure` or contains `(` followed by `try`, and no existing form changes meaning). 436
  call sites by name when the duals go (`TryParse` 300, `Convert.TryTo*` 79, `ParseOrNull`
  53, `ReadKeyOrNull` 4).

### Comparison

| | A | B | C | D | E | F |
|---|---|---|---|---|---|---|
| New language surface | 1 form | clause, call mark, failure-typed functions and tasks | operator, async container, owning combinators | none | B's | 1 interface, 1 form |
| New ownership rules | 0 | payload restriction; task and domain rules | owning-match propagation; container | 0 | B's | none identified (§5.2); the atom's result channel is not modelled |
| Stdlib members made redundant | 104 | 120 | 0 (adds duals) | 0 | 120 | marker: 0; atom: 31, and later duals over non-nullable results |
| Corpus sites broken | 0 | ≥ 1,575 throws + 3,193 catch blocks to re-band; every failing call marked | the same sites rewritten as returns and matches; blocked (0 of 4 conditions) | 589 to 1,780 catch arms | B + D | 0, then 436 mechanical |
| Hides bugs | yes (p12) | no | no | no | no | no (p16) |
| Keeps cleanup on the bug path | yes | yes | yes | **no** | **no** | yes |
| Failure type survives a domain seam | no | yes | value-like only | — | yes | band only (§8.4), type no |
| Protections of §3 given up | 4, 5 | 9 | 5 (reversed: values everywhere), plus the unmet bar | 1, 4's catchable contracts | B's and D's | marker: none; atom: 5 |

### Design sheet

The candidates again, one row per question the brief asks. The B and C columns describe
designs that do not exist in Bezel; their cells are sketches (HYPOTHESIS), not measurements.
E is B and D together and has no column.

| | A blanket `try?` | B typed throws | C `Result`-first | D abandonment | F |
|---|---|---|---|---|---|
| Syntax | `try? E` | `method M(): T throws E`; `try M()` at each call; `try? M()`; `try … catch` | `Result<T, E>` returns; a propagation operator spelled outside the `?` family | none added; `catch` of bug types removed | `(try E)`; a marker interface |
| Typing | `T?`; any exception converts | the member's type carries `E`; the caller must handle or itself throw a superset; function types and generics need a failure parameter | ordinary generics; an instantiation with an owned payload is move-only | — | `T?` (or `bool` for a unit operand; a nullable operand is an error); a failure is an exception whose class implements the marker |
| Error set | undeclared | declared on public members | declared in the return type | undeclared | undeclared per member; the band is per type |
| Ownership on the error path | today's throw edge | the return edge, which disposes the same values (p02); payload must be value-like, no borrows | owning match; propagation is a consume of the payload | none: nothing is disposed | today's throw edge; no new rule |
| Async | task faults, as today | the task must carry `E`: a second type parameter or an erased failure | no container for an owning `Result` (p07) | the domain or process ends | as today |
| Domain seam | `DomainFaultedException` | a value-like `E` crosses with its type | a value-like `Result` crosses (spec §35.12); an owning one does not | — | the band crosses (§8.4); the type does not |
| FFI boundary | abort (spec §32.8) | same; `extern` members cannot fail, wrappers convert `errno` (spec §32.9) | same | same | same |
| Exhaustiveness | none | possible where `E` is a choice | yes (spec §35.6) | — | none; open, like `catch` |
| Evolution of error sets | open | a new case breaks exhaustive handlers; a wider `throws` breaks callers | "every case addition breaks every exhaustive consumer" (QUOTED, stdlib §1) | — | a new failure type breaks nothing; moving a type between bands is a governed behaviour change |
| New diagnostics | none | unmarked failing call; unhandled failure | non-exhaustive match (exists) | — | owned argument lost on failure; nullable operand rejected |
| Run-time cost | one exception object per failure (73 ns on r40) | a value return, no allocation | a value return, no allocation | lowest | as A; a lowering for known members removes it |
| Binary size, compile time | unchanged | a check per failing call, which r40 already emits per call (13% of the call instructions in p02's object) | tag tests | smaller | unchanged; compile time not measured |
| Removes | 104 stdlib members | 120 stdlib members; `Result` as a signal | nothing (bugs still throw) | catchability of about 15 types | with the atom: 31 stdlib members; future duals over non-nullable results |
| Migration | none | re-band and mark every site, at an edition | blocked (§9.2) | tests and supervisors rewritten, at an edition | additive, then one warn-first removal |

## 5. Ownership interplay

### 5.1 What holds today (the base every candidate inherits)

- **`throw x` is a kill the keyword announces.** The operand is consumed; ownership passes to
  the runtime; "the runtime's dispose-after-arm sets the disposed flag" (QUOTED, spec §31.1).
  Probe p11 (MEASURED): an exception that owns a payload disposes it right after the arm.
- **The throw edge and the return edge run the same cleanup in the same order.** Probe p02
  (MEASURED): `close c, close b, finally, close a` on both. Model (MEASURED,
  `model/to_null_model.py`, pinned at v0.345): 3,927 pairs of programs, one leaving a scope by `throw`, its twin
  by normal exit, have identical disposal traces; with K3.15 switched off, 1,386 differ.
- **The implementation already propagates errors as values.** r40 stores the exception in a
  thread-local slot and returns; each call site that can see a throw then tests the slot
  (QUOTED: `Emitter::emitPendingCheck`, `src/codegen/LlvmBackend_Disposal.cpp`; `bz_exc_addr`,
  `src/runtime/bezel_runtime_itable.cpp`). MEASURED on probe objects: no `.gcc_except_table`
  section; in `p02` 42 of 327 call instructions are the slot helper. So "unwinding versus value
  returns" is not a choice Bezel still has to make at the implementation level; the cost of a
  thrown failure is the exception object, not a table walk.
- **A caught exception is a borrow** (probe p05, MEASURED: assigning it to an outer local is
  rejected with the §31.1 diagnostic). Nothing downstream can hold the error.
- **An exception carrying a borrow** is admitted under residual trust and faults in checked
  disposal mode if the source died in the unwind (QUOTED, spec §31.1). "Exception classes
  should therefore carry owned or snapshot state."
- **One in-flight slot, later wins.** Probe p13 (MEASURED): a throwing `finally` replaces the
  failure being propagated; the first failure is not catchable. (`SupersededChain` is not
  implemented in r40.)
- **A task owns its fault; awaiters borrow it.** Probe p03 (MEASURED): the frame's `using var`
  is disposed on the fault edge, and two awaiters of one task both see the fault.
- **A domain boundary erases the type and the band.** Probe p04 (MEASURED): a
  `FileNotFoundException` and an `IndexOutOfRangeException` both arrive as
  `DomainFaultedException`; arms naming the original types do not match; the bug arrives with
  an empty `Message`.

### 5.2 Design F on each point

The marker changes nothing here: it is an interface on exception classes and an arm type the
language already admits. The rest of this section is about the deferred atom, defined once in
§8.1 item 3 as an expression form of its own. No new ownership rule was identified for it;
that is an argument from the existing rules below, not a model result.

| Question | Answer under F's atom | Evidence |
|---|---|---|
| Disposal on the failure path | the callee's throw edge has already disposed its locals; the exception is disposed at the atom as after a completed arm | spec §31.1; p11, p15 |
| An owned result | `(try new Conn(h))` is a fresh producer of `owned Conn?`; it binds to `using var`, narrows, and is disposed on every path | p15 (MEASURED on the equivalent helper call, no warning) |
| `throw x` as a kill | unchanged; the atom adds no consume | argued from spec §31.1; not modelled |
| `using var` and `finally` ordering | unchanged; the atom adds no cleanup entry | p02 |
| An owned *argument* to the failing call | consumed by the callee and disposed on its failure edge: the caller gets `null` and has lost the argument. This is the give-back problem of stdlib §6.9; it stays with proposal 0060. A warning at `(try F(move x))` is proposed | QUOTED, stdlib §4.9 `OwningDictionary.Add`: "the passed value was consumed at the call site and is disposed on that path" |
| A failure carrying an owned payload | allowed as today; disposed with the exception when the atom yields `null` | p11 |
| A failure carrying a borrow | as today (residual trust); the atom shortens the exposure, since the exception dies at the atom | spec §31.1 |
| Async | `(try await F())` behaves as a `try` around an `await`, which spec §30.9 admits anywhere in a body; the task-owned fault is not disposed by the arm (the §30.9 exemption) | spec §30.9; p03 |
| Cancellation | `OperationCanceledException` is not a failure, so the atom never swallows it | by construction |
| Task and domain seams | types still do not cross. The band can: §8.4 | p04 |
| A nullable operand | rejected: a failure and a legitimate `null` would be one value, the distinction stdlib §1's shape (a) exists to keep | stdlib §1 ("the only shape that distinguishes the outcomes a collapsed `T?` cannot") |
| The result as an unbound temporary | under `??` it is disposed correctly today; as the receiver of `?.` or `!` r40 does not dispose it, where spec K8.1 says it must | p19 (MEASURED) |

**What the model does and does not show** (MEASURED; `model/to_null_model.py`,
`model/out_vars3_len3_body2.txt`). The model is bzcore-l1, pinned at spec **v0.345**, ten
revisions behind the text this report reads. It has locals, pins, fields, temporaries and
`throw`; it has no calls, no values, no `null`, and its catch arm catches everything.

- *Existing `try` with an empty catch-all.* Of 333,648 programs of the shape *prefix; try
  { body } catch e { }; suffix*, the checker accepts 77,559, and none violates exactly-once
  disposal, use-after-kill or isolation.
- *An outer owned local assigned inside the protected block* (the statement reading of the
  atom's result, with a nondeterministic branch standing for an operand that either fails or
  produces): of 19,565 programs *decl r; prefix; try { if \* { throw } else { r := new R } }
  catch e { }; suffix*, the checker accepts 4,497 with 0 violations; a later read of `r` is
  rejected (K1.3), which is where the real form's null test would go.
- *Not modelled:* the null result as a value; the band filter; a call that fails after
  consuming an owned argument; `await`; the temporary positions of p19. A model of the atom
  proper is an entry condition of the gate in §8.5.

### 5.3 The other candidates on the hard points

- **B/E.** A typed error that can be copied to several awaiters and sent to another domain must
  be value-like and hold no borrow; so "owned payload in an error" and "error carrying a
  borrow" become compile errors. That is a real loss against p11's shape, and a real gain at
  the seams. Disposal on the error path is the return edge, which p02 shows is already the
  same.
- **C.** An owning `Result` is a move-only value: consumed only by owning match, not a task
  result (p07), not sendable (spec §35.12). A value-like `Result` is sendable, which is the
  spec's own advice for cross-domain failure data ("return it as a sendable result", spec
  §30.14) — though r40 emits malformed IR for it (p08).
- **D.** No unwinding means no disposal: sound only if the whole process ends.

## 6. Prior art

From memory; no source was consulted during the run. Each entry ends with what it teaches
Bezel.

- **Rust.** `Result` plus `?`, `Option` for absence, panics for bugs (unwinding by default,
  caught only at thread boundaries or `catch_unwind`). Both `v[i]` (panics) and `v.get(i)`
  (`Option`) exist; `.unwrap()` converts a value into a panic. Applications converge on an
  open error type (`Box<dyn Error>`, `anyhow`) at boundaries and typed errors at leaves.
  *Teaches:* two bands; query/assert pairs survive even in a `Result`-first language; the
  band-3/band-4 split of stdlib §1 is what Rust practice arrived at.
- **Swift.** `throws`, with `try` (propagate), `try?` (to optional), `try!` (assert); errors
  are values; bugs trap. Typed throws arrived in Swift 6 with advice to use it sparingly;
  untyped `throws` stays the default. *Teaches:* one declaration plus caller-chosen stances
  removes API duals; `try?` is safe there only because bugs are not errors.
- **Zig.** Error unions, `try`, `catch` with a default, `errdefer`, inferred error sets.
  Errors carry no payload, so detail travels through out-parameters. *Teaches:* keyword
  spellings avoid operator-token collisions; payload-free errors push information elsewhere.
- **Go.** `(T, error)` returns and `if err != nil`; `panic`/`recover` for bugs; nothing forces
  a check. The team stopped pursuing syntax for it after years of proposals. *Teaches:* a
  uniform value convention without a propagation form is verbose enough that users keep
  asking for one.
- **C# and Java.** Java's checked exceptions failed on versioning (adding one breaks callers),
  on scale (lists aggregate through layers) and on lambdas (no way to abstract over what a
  function throws). C# has none, and grew `TryParse` beside `Parse` because a thrown failure
  was slow. *Teaches:* Bezel's `Try` tier is that pattern; a forced-handling clause is the
  trap, a marker is not.
- **Kotlin.** Unchecked exceptions; the `xOrNull` naming convention across the library;
  `runCatching` catches everything, including cancellation, a known source of bugs.
  *Teaches:* the `…OrNull` tier's origin; a to-null form must exclude cancellation and bugs.
- **OCaml.** Exceptions are cheap, yet `find_opt` was added beside `find` so that absence
  shows in the type. *Teaches:* duals appear for visibility, not only for speed.
- **Koka and effect systems.** Failure is an effect in the function type; handlers turn it
  into a value; effect polymorphism lets `map` take a failing function. *Teaches:* the fully
  general "one mechanism" exists, and its price is effect polymorphism across the type
  system; candidate B is a partial version and inherits the lambda problem.
- **Midori.** Bugs abandon the process; recoverable errors are declared, marked with `try` at
  the call, and convertible in place (`try F() else v`, a form yielding a `Result`). Contracts
  and abandonment outnumbered exceptions about ten to one; the `TryX` pattern was dropped.
  Abandonment was affordable because processes were small and isolated. *Teaches:* the
  two-band model and the caller-side conversion are the parts that transfer; process-level
  abandonment does not, without that isolation.
- **P0709 (Sutter).** `throws` functions return a small error value through the return
  channel; bugs go to contracts; allocation failure fails fast. *Teaches:* the same two bands;
  and the value-return implementation Bezel's compiler already has.
- **Hylo.** Value semantics, with a Swift-like throwing model still being designed (low
  confidence). *Teaches:* an error that must be self-contained cannot carry a reference.
- **Odin and Jai.** Multiple returns; Odin's `or_return` and `or_else` are keywords. No
  exceptions. *Teaches:* keyword operators again; nothing enforces a check.
- **Ada and SPARK.** Unchecked exceptions with a message; contracts raise `Assertion_Error`;
  SPARK proves run-time errors absent and (recently) lets a subprogram declare its exceptional
  cases as a contract. A task's exception reaches its caller as `Tasking_Error`, type lost.
  *Teaches:* failure declarations fit the contract family; type erasure at a task seam is a
  known, accepted design.

No language in this list stayed with one mechanism. The exception-only ones (C#, Java,
Kotlin, OCaml) grew value-returning duals; the value-only ones (Go, Rust, Zig, Odin) kept a
panic. Rust, Swift, Zig, Go, Midori and P0709 each settled on two bands, with a one-way
conversion from the handled band into the bug band (`unwrap`, `try!`, `panic`). Bezel has
that conversion (`!`, `?? throw`) and lacks its converse.

## 7. Probes

r40, default flags; sources in `probes/`, output in `probes/RESULTS.txt` (regenerate with
`probes/run_all.sh`). All results MEASURED and informational: the compiler implements an older
revision than the spec by design.

| Probe | Shows | Result |
|---|---|---|
| p01 | one lookup, three tiers; `?? throw` and `!` rebuild the assert stance; the value tier does not convert a caller bug | runs; `!` on null and `Parse` report an empty `Message` |
| p02 | throw edge and return edge dispose identically | runs; identical order |
| p03 | exception after `await`; frame disposal; two awaiters | runs |
| p04 | exception out of a domain | runs; type and band lost |
| p05 | storing a caught exception | rejected (§31.1), as specified |
| p06 | value-like `Result`; one statement-`case` per propagating call; combinators | runs |
| p07 | `Task<Result<Connection, string>>` | rejected (§30.5, §35.4), as specified |
| p08 | a value-like `Result` returned from a domain | **compiler error: malformed IR** (spec §35.12 admits it) |
| p09 | cost of one failure per tier | `TryParse` 7.5 ns; `Parse`+`catch` 73 ns, 133 ns through 8 frames; `TryGetValue`/`GetOrNull` 3.5 ns; indexer+`catch` 72 ns; success 8 ns either way |
| p10 | uncaught exception | disposals run, report printed, exit 70 |
| p11 | exception owning a payload | payload disposed after the arm |
| p12 | a to-null helper written today: blanket vs named | blanket hides a bug; named does not |
| p13 | throwing `finally` | later wins; `SupersededChain` not implemented |
| p14 | contract violation, index fault, bad input | one `Exception` arm takes all three |
| p15 | a helper standing for `(try new Conn(h))`, catching the marker, over an owned result | runs, no warning, disposed on every path |
| p16 | the band marker as an interface, with an interface catch arm | runs: failures become null, the bug propagates, `catch OverflowException` still matches |
| p17 | `ParseOrNull` on `bool`, `Guid`, `int`, `long`, `double` | the three numeric calls are rejected: the member does not exist on r40 |
| p18 | what a marker-typed catch binding can read | runs; `ToString()` yes, `Message` no (two rejected variants recorded in the file) |
| p19 | the atom's positions with today's forms: an outer owned local assigned in the protected block; the nullable owned temporary under `??`, `?.`, `!` | outer local and `??` dispose correctly; under `?.` and `!` the temporary is **not disposed** (a non-nullable receiver is) |

## 8. Recommendation

**Adopt the failure band now (stages 0–1). Hold the atom (stage 2) behind the gate in §8.5.**

This is the staging the non-author review of this report advised, and it is now the
recommendation. The first version of this section recommended F whole. §8.2 gives the two
options side by side and the reasons for the change.

### 8.1 The design

1. **Three kinds of outcome, one spelling each.**
   - *Absence*: a query whose answer may be "nothing there" returns `T?`, or a `Try` shape
     where the caller must bind and test in one step or tell absent from present-null. Not
     error handling.
   - *Failure*: an operation that may be unable to do its job throws an exception whose class
     implements the failure marker.
   - *Fault*: a bug. `requires`, a run-time check, or a `throw` of any exception that does not
     implement the marker.
2. **The failure marker** (working name `IFailure`): a built-in interface (its members, if
   any, are open question 1 in §11), implemented by `FormatException`, the `IOException`
   family, `TimeoutException`, `SerializationException`, `ChannelClosedException`,
   `ChannelFullException` and their proposed peers. `OperationCanceledException` does not
   implement it. An exception type's band changes only by recorded ruling, as taxonomy growth
   does today (spec §31.4). An interface is chosen over a base class because spec §31.2.1
   already admits interface-typed catch arms (pinned by
   `tests/lit/sema/sema_catch_arm_type__interface_arm_admissible_31_2_1.bzl`) and because a
   class can join the band without leaving its base (p16).
3. **The to-null atom** `(try E)` (stage 2, gated). Defined here once.
   - *Syntax.* A paren atom in the family of `(case …)`, `(if …)` and `(var x := E; body)`:
     `(` then `try` is a two-token signal no expression has today (Appendix A uses `"try"`
     only in `TryStmt`). It uses no `?` token, so the four-way collision recorded for a
     propagation operator (rationale §11.4, proposal 0076 §3) does not arise.
   - *Definition.* An expression form of its own. It cannot be defined by expansion: a
     `try` statement cannot be written where statements are not allowed, and the binding atom
     (spec §17.15) admits no statements. Evaluate `E`. If `E` completes normally, the atom's
     value and ownership classification are `E`'s (as spec §17.15 says of a binding atom's
     body), at the nullable type: where `E` is a fresh owned producer, so is the atom. If
     evaluation of `E` raises an exception whose class implements the marker, handling of
     that exception completes at the atom — it is disposed as after a completed catch arm
     (spec §31.1, with §30.9's exemption for a task-owned fault) — and the atom's value is
     `null`. Any other exception propagates.
   - *Statement reading* (an explanation, not the definition). Where the atom is the whole
     initializer of a local, it reads as

     ```bezel
     var r: T? := null;                  // plain `var`: a `using var` cannot be assigned later
     try  r := E;  catch IFailure => ()  end try;
     ```

     which compiles and disposes correctly on r40 (p19 (1)), with the must-use warning the
     plain `var` draws. p15's helper method is the same reading wrapped in a call.
   - *Type.* `T?` where `E: T` and `T` is not nullable; `bool` where `E` has the unit type
     (`true` on completion); a **compile error** where `E: T?`, because the failure and a
     legitimate `null` would be one value.
   - *Positions.*

     | Position | Admitted | Note |
     |---|---|---|
     | initializer of `var` / `using var` | yes | p15, p19 (1) |
     | operand of `??` | yes | the equivalent call disposes correctly today (p19 (2)) |
     | receiver of `?.` or `!`, e.g. `(try new C(h))?.M()` | yes | an unbound owned temporary, disposed at the end of the full expression (spec K8.1); r40 does not dispose it today (p19 (3), (4)): a compiler gap to close first |
     | argument; expression-bodied member (`=> (try int.Parse(s)) ?? 0`) | yes | an expression like any other |
     | around `await` in an `async` body | yes | spec §30.9 admits `await` in a `try` body |
     | contract clause | no | a clause states a proposition and already rejects `throw` (spec §22.16) |
     | `pure` member body | open | §11 question 3 |

   - *Diagnostics.* A warning where the operand passes an `owned` argument (lost on failure);
     the error on a nullable operand; a hint for `(try E)!` (write `E`).
   - *What it gives up.* It is a catch at an interior call site whose result the program
     branches on. Guide 2 §7 calls that "the signal to check the rule for a value-shaped
     tier". The atom reverses the sentence: it *is* the value-shaped tier. Protection 5 of §3
     does not survive it.
4. **`catch` gains a boundary arm** with the marker alone: `IFailure => …` takes every
   expected failure and lets bugs reach the supervisor, where `Exception => …` takes both.
5. **The straddling types, ruled.** Three types are raised for bugs and for conditions a
   correct program can meet (§10.2). The library names them in 48 type-and-section pairs
   (`ArgumentException` 19 sections, `OverflowException` 9, `InvalidOperationException` 20;
   MEASURED). Each condition is ruled once:
   - *Out-of-range input to `Parse` and `Convert.To*`* (`OverflowException`, stdlib §9.1,
     §2.3): a failure. It gets a class that derives from `OverflowException` and implements
     the marker, so existing `catch OverflowException` arms keep matching (p16).
   - *"The collection is empty"* (`First`, `Single`, `Pop`, `Dequeue`, …:
     `InvalidOperationException`), *duplicate key* (`Add`: `ArgumentException`), and with
     them the missing key (`KeyNotFoundException`) and the bad index
     (`IndexOutOfRangeException`): **stay faults**. Stdlib §1 already says the throwing form
     "treats absence as a bug", and each has a query dual that is kept (`FirstOrNull`,
     `TryPop`, `TryAdd`, `TryGetValue`, `ElementAtOrNull`). The consequence is stated plainly:
     `(try list.First())` does **not** yield `null` on an empty list; the exception
     propagates, and the value form is `list.FirstOrNull()`. The same holds for 63 of the 89
     measured families and for the three dictionary indexers: each keeps its two forms.
   - *Every other use* (state misuse, bad arguments, arithmetic overflow, the `DateTime`
     range fault): a fault, unless a ruling says otherwise.

   The price beyond `Parse`: 48 pairs to read once; no behaviour change where the ruling is
   "fault"; one derived class per condition ruled a failure. Making the query conditions
   failures instead would need a derived class for each of the 63 families' conditions and
   would let the atom replace their duals; that is the `fails`-clause direction of §8.3, not
   recommended.
6. **The standard library, if the atom is adopted.** Stop adding duals for operations whose
   result is not nullable, and retire the 31 Parse-family duals behind a warn-first ramp (the
   Ratchet class of spec-change-process §2; an edition, if proposal 0108 has landed by then —
   it is a draft today). An operation whose success value may be `null` keeps a shape-(a)
   `Try` form or is handled with `catch`.
7. **`Result<T, E>` stays, as data.** It is for outcomes a program stores, gathers or sends
   (validation lists, fan-out results), which is where 0076 §3 found it better outright.

### 8.2 The two options, and which is recommended

**Option 1 — the band now, the atom gated (recommended).** Stages 0–1: the marker, the
boundary arm, the straddler rulings, the three-kind teaching rule. The atom waits for the
gate of §8.5.

**Option 2 — F whole, now.** Stages 0–2 together: the atom lands with the marker, D-0005 is
reopened, and the freeze on duals starts at once.

Why option 1:

- *The marker is useful alone and is a prerequisite either way.* It works as a library
  convention today (p16). It gives boundaries an arm that does not take bugs; the corpora
  have 641 bare-`Exception` arms, each of which takes bugs with failures. It reopens nothing
  and gives up no protection of §3. The atom cannot be sound without it.
- *The atom's evidence is the wrong class.* Everything measured in its favour is a count of
  library declarations and test programs. No observed program needs it. Proposal 0061 §2's
  gate asks for ceremony "observed, not predicted", and 0076 §0 classed its own authored
  programs as "predicted evidence"; the same discipline applies here.
- *The atom reopens a decline that was reaffirmed.* 0076 §7 records D-0005 as "Untouched and
  *reinforced* — on both grounds", and the atom gives up the catch-at-boundary doctrine 0076
  §6 made explicit. The case in §9.1 answers the grounds with a cost argument; it is not
  evidence about use.
- *The model does not reach it.* The result channel is unmodelled (§5.2), and one of the
  atom's positions leaks on r40 (p19).
- *Waiting costs little.* The duals stay; tier growth continues by proposal, as today. Each
  new dual's proposal can record whether the atom would have made it redundant, which is the
  evidence log the gate needs.

Why not stop at option 1 forever: without the atom a caller still writes four lines to get a
`null`, so "state each operation once" is not livable and the pressure for per-member duals
stays. Option 1 is a first step with a stated next one, not the whole answer to the owner's
question.

### 8.3 Why not more

A declared `fails(T, …)` contract clause per member (in the `requires` family) would be more
precise and would let the query/assert pairs be derived too. It is not recommended: it
touches spec §31.3, needs conformance rules for overrides and interface members, gives lambdas
and task variables no declaration to read, and leaves cancellation to each author's care. It
is the natural next step if per-member precision or typed failure sets are wanted, and nothing
in F blocks it.

### 8.4 The domain seam

Today a failure and a bug leave a domain as one type (p04). Under F the join can keep the band
without sending the object. A class's interfaces are fixed, so one type cannot implement the
marker "when the original did": the join raises a subclass, say `DomainFailedException:
DomainFaultedException, IFailure`, when the original exception's class implemented the marker,
and `DomainFaultedException` otherwise. Existing `catch DomainFaultedException` arms match
both. One bit crosses; no exception class becomes sendable, so the two declines on that point
stand (decline-index rows *Sendability requirement on throwable types* and *Stating
`Exception`'s inherited state transfer-compatible*).

### 8.5 Order of work, and the gate

> **Withdrawn (2026-11-11, owner decision "Withdraw; OrNull is doctrine"; proposal
> [0214](../../proposals/0214-narrowed-failure-marker.md) §6, v0.358).** Stages 2 and 3 below
> are withdrawn as a direction, not deferred. The owner observed that a call-site form naming
> the exception types it turns into `null` (`(try int.Parse(s) on FormatException)`) is longer
> and less precise than the `*OrNull` member it was meant to replace, and a form over a default
> set (every `IFailure`) swallows conditions the caller never meant to tolerate — after v0.358's
> narrowing it would not even cover `FormatException`. The `*OrNull`/`Try` tiers are the
> doctrine for value-shaped results: the member's author, who knows its expected-absence
> condition, states it by providing the dual. The one shape a future proposal may bring back is
> a *declared-absence* form (each throwing member declares once which exception means "no
> result", and a call-site form reads that declaration); its bar is recorded in `DECLINED.md`.
> A member with an expected failure and no value-shaped form is a standard-library gap, filed
> as its own issue. The table and the gate below are kept as the record of what was proposed.

| Stage | Content | Class (spec-change-process §2) | Depends on |
|---|---|---|---|
| 0 | Compiler catch-up the probes exposed: numeric `ParseOrNull`, `SupersededChain`, `Exception.ToString()` message, `FaultStackTrace`, the p08 crash, the undisposed nullable temporary of p19 | implementation | — |
| 1 | The failure marker; the band governance sentence; the 48 straddler rulings and the Parse-range class; Guide 2 and stdlib §1 restated around three kinds; `IFailure => …` taught as the boundary arm | design change (proposal) | owner |
| 1b | `DomainFaultedException` keeps the band (§8.4) | ruling | 1 |
| **gate** | see below | | 1 |
| 2 | The `(try E)` atom; D-0005 reopened; rationale §11.4 rewritten; no new duals over non-nullable results | design change (proposal) | the gate |
| 3 | Retire the 31 Parse-family duals; lower `(try X.Parse(s))` to the existing non-throwing entry first | deletion-shaped proposal, warn-first ramp | 2, and the lowering |
| later | A `fails` clause; value-like failure payloads for the decode cluster (0076's pilot) | evidence-gated | real programs written with stage 2 |

**The gate for stage 2.** All of:

1. *Observed use.* Programs not written to test the compiler (the sibling ergonomics study's
   corpus counts; later, external users) show callers hand-writing the to-null shape — a
   `try` whose arm only produces a fallback, or a helper like p15's — or asking for duals the
   atom would derive, often enough to matter. The dual proposals filed after stage 1 are the
   log.
2. *A model of the result channel.* The ownership model, brought to the current revision,
   covers a failing call, the null result, the band filter and an owned argument, with no
   violation.
3. *The temporary positions work.* The p19 gap is closed.
4. *An owner ruling* that the atom's use is an accepted exception to "catch at boundaries",
   with the evidence of §9.1 laid against D-0005's grounds.

### 8.6 Proposal outline (not filed)

In the shape of recent graduated proposals (header block, then numbered sections).

*Proposal X — The failure band* (stage 1).

- **Header.** Status: draft. Tracking issue: none. Target: spec §31.1–§31.4, §30.14 *Faults*;
  stdlib §1, §2.17, §7.5, §9.1 and the sections that name a straddling type; Guide 2;
  code-generation reference §5.1, §6. Written against: v0.355.
- **0 Summary.** The first sentence of the rule in §1; one interface; no syntax.
- **1 The text today.** Stdlib §1's five bands; spec §31.2.1's interface arms; spec §31.3.
- **2 The problem, measured.** §2.3's band table: 641 bare-`Exception` arms; 1,191 arms on
  straddling types; p14.
- **3 Options.** An interface, a base class, a throw-site band keyword, a `fails` clause.
- **4 Evaluation.** Against the nine protections of §3 (none given up); p16, p18.
- **5 Reach, measured.** 0 corpus sites; the 48 straddler pairs; the Parse-range class.
- **6 The case against recorded declines.** None is reopened.
- **7 Declined alternatives** (rows for the decline index): abandonment; forced handling;
  re-typing the query conditions.
- **8 Normative edits, changelog, migration, trackers.** One revision; spec-ahead trackers;
  a conformance case per ruled condition.
- **9 Self-critique.** Open questions 1 and 4 of §11.

*Proposal Y — The to-null atom* (stage 2, only after the gate): the definition of §8.1 item
3; the gate's evidence; §9.1 as its section 6; declined alternatives (blanket `try?`,
`(try E else v)`, `(try E catch …)` arm forms, a nullable operand admitted with a warning).

*Proposal Z — Retire the Parse-family duals* (stage 3, deletion-shaped): the 31 declarations;
the 436 corpus call sites by name and the rule cases that pin them; the `IParsable<T>.TryParse`
witness; the lowering precondition; the ramp, both ends stated.

## 9. Recorded declines and gates engaged

### 9.1 Decline D-0005, the value-form try expression

This section applies to the deferred atom only. The marker reopens nothing.

QUOTED (`docs/proposals/DECLINED.md`, D-0005): the form "fails test 3: it makes
exception-based *expected* failure ergonomic, precisely the path Guide 2 §6 … routes to
`T?`/Try/`Result`; the doctrine would be undermined by its own syntax, and the borrow-scoped
catch model (§31) offers no owned exception value to carry anyway. Revisiting requires new
evidence against these reasons."

The decline has been reaffirmed since. QUOTED (proposal 0076 §7): "Untouched and
*reinforced* — on both grounds. Ground 1 (it would make exception-based expected failure
ergonomic, against Guide 2 §6's routing) is now backed by a normative rule (§5) rather than a
table; ground 2 … is engaged at §2(a) as a permanent structural fact both candidates must
design around." And 0076 §6 made the doctrine behind ground 1 explicit: "`catch` is a
*boundary and supervision* tool — convert, log, retry, report, re-raise — and … branching
program logic on caught types at interior call sites is the signal to check the failure-signal
rule for a band-2/3 tier" (QUOTED; landed as Guide 2 §7's sentence, §3 item 5).

`(try E)` is the smallest member of the declined family: no `catch` arms, no `else`. What can
be said for it, and what cannot:

- *Against ground 1.* (a) The doctrine's cost was not measured when it was declined or when
  it was reaffirmed: 120 hand-written value-tier members, 89 duplicate families, 15
  failure-documenting sections with no value-shaped form, 71 changelog revisions and 17
  proposals that touch tier spellings (§2, MEASURED). (b) The declined form was band-blind,
  because no band marker existed; it would have converted bugs (p12). `(try E)` converts the
  failure band only (p16). (c) The project already accepts call-site conversion in the other
  direction: enum name parsing shipped the value form only, with the throwing tier declined
  pending "Measured `?? throw new …` boilerplate at scale in real programs" (QUOTED,
  decline-index row *Enum name-parse tiers*).
- *What is not kept.* The atom is an interior catch whose result the program branches on. It
  keeps half of the doctrine — the caller branches on a value, and writes no `catch` arm —
  and gives up the other half: under it an interior catch is no longer the signal of a
  missing value tier, because the atom is that tier. That is protection 5 of §3, given up
  wherever the atom is used. An earlier draft of this section said the doctrine's purpose was
  kept; that was wrong.
- *Against ground 2.* The atom carries no exception value. The exception is disposed at the
  atom, as after an arm. This ground is answered.
- *The honest limit.* Points (a)–(c) are a cost argument and a design difference. They are
  not evidence about how programs consume failures, which is what "new evidence against these
  reasons" asks for, and proposal 0061's admission test 3 ("does not make a doctrine-declined
  path more ergonomic") is still failed. The atom can land only if the owner changes the
  doctrine from "expected failure is a value in the API" to "expected failure is a value at
  the call", on the evidence the gate of §8.5 collects.

### 9.2 Proposal 0076 §6, condition by condition

| Condition | Met? | Evidence | What would meet it |
|---|---|---|---|
| 1. A propagation operator landed through 0061 §2's gate, `?` collision resolved | **No** | 0061 §2 is still "evidence-gated"; decline-index row *`Result<T, E>` propagation in expression position* is `deferred` (QUOTED). `Result<` appears in 49 of 13,522 corpus files, all tests (MEASURED) | observed per-call ceremony in real `Result` code written with 0075 stages 1–2, and a spelling outside the `?` family (a prefix keyword) |
| 2. A single-award async container for move-only value results | **No** | spec §35.4 (v0.355): a `Task<T>` "declared over an owning choice is a compile error" (QUOTED); probe p07 rejected (MEASURED) | its own proposal: the container, and spec §35.5's trigger list extended to `await` results |
| 3. The payload governance rule has operated at stdlib scale | **No** | the rule is normative text in stdlib §1 (QUOTED), but the library declares 0 error-payload types and 0 `Result`-returning producers (MEASURED) | shipped payload types whose case sets have had to grow |
| 4. "real programs consuming the decode-cluster Result surfaces" show "branch-shaped consumption dominating boundary-shaped consumption on the frontier surfaces", "plus the migration/compatibility analysis" (QUOTED, 0076 §6) | **No** | there is no decode-cluster `Result` surface to consume: the pilot has not landed (stdlib §2.15, QUOTED in §2.2), and "There are no external users yet" (QUOTED, `CLAUDE.md`). So the branch-versus-boundary test cannot be applied to data. The corpora's catch arms are test oracles and say nothing about it. §2.3 supplies site counts towards a migration analysis, on test corpora only | the pilot; then programs that consume it, with their consumption sites classified branch or boundary; then the compatibility analysis |

Neither the marker nor the atom depends on any of the four, and neither reopens the question.

### 9.3 Proposal 0061 §2

The gate is for an operator that propagates a `Result` out of an expression. The deferred
atom is not one: it removes an exit edge instead of adding one, so the "announced flow"
objection does not apply, and it has no `?` token. The gate is untouched and stays closed.
Its discipline ("observed, not predicted") is the one §8.5 borrows for the atom.

### 9.4 Other rows touched

- *`Result`-first error model* (rejected, rationale §11.4): not re-proposed.
- *`Option<T>`* (D-0004): untouched; `T?` stays the only spelling of absence.
- *Catchable allocation failure*; *`defer`/`errdefer`*; *primary-wins supersession*;
  *`Result` extractors (`Unwrap`)*: untouched. F asks no "are we unwinding?" question.
- *`KeyNotFoundException` deriving from `InvalidOperationException`* (rejected so that "a
  state-fault catch arm does not silently swallow missing-key lookups"): the same reasoning,
  applied to bands, is the case for the marker.
- *Forcible domain teardown* (rejected): the ground on which D and E are not recommended.
- *Changing `TextReader.Read()` to return `char?`*: untouched. The marker re-rules one landed
  condition's class (out-of-range `Parse` input gains a derived class, §8.1 item 5); the 31
  duals move only by their own proposal, after the gate.
- Rationale §11.3 (`catch` is `case` for exceptions), §11.7, §11.8, §11.10: unchanged.
- Rationale §17.11 and §18: nothing deferred there is needed.

## 10. Spec defects, ambiguities and compiler gaps found

Not filed. Spec and stdlib items first.

1. **`DomainFaultedException` drops the original type name.** Spec §30.14 and stdlib §6.8
   carry `Message` and `FaultStackTrace` only, while `ExceptionInfo` (spec §31.4) has
   `TypeName`. A fault with an empty message crosses with nothing that identifies it (p04).
   Candidate fix: populate `CauseChain` with the original's snapshot.
2. **One exception type in two bands.** `OverflowException` is the arithmetic fault (spec
   §31.4) and the `Parse` family's input failure (stdlib §9.1); `InvalidOperationException`
   is state misuse and "the collection was empty"; `ArgumentException` is a caller bug and a
   duplicate key. The failure-signal rule classifies conditions, so the text is consistent,
   but nothing keyed on a type (a catch arm, a lint, a band marker) can tell them apart.
   1,191 of the corpora's 3,482 catch arms (34%) name one of the three (MEASURED), and the
   library names them in 48 type-and-section pairs. §8.1 item 5 rules on them.
3. **Spec §31.3 versus the library's practice.** The spec names the `<exception>` tag as the
   convention; the standard library uses it nowhere and states fault contracts in prose.
4. **Stdlib §1's tier vocabulary does not fit shape (b).** `TryAcquire`'s twin `Acquire`
   blocks; it is not "the throwing form". The no-dual rule's definition of a tier (assert
   stance beside branch stance) does not cover blocking beside non-blocking.
5. **Guide 2 §7 is narrower than stdlib §1.** The guide routes "absent and owned" to
   `bool` + `out owned T?` only; stdlib §1 shape (b) returns `owned T?`, and p15 shows an
   `owned T?` return binding to `using var` and narrowing with no ceremony.
6. **No stated cost model** (already noted by 0076 §2(h)): the `Try` tier's usual
   justification is speed, and no document says so or bounds it. On r40 the gap is 10–20×
   on the failure path and nil on the success path.

Compiler (r40) gaps observed, informational: `ParseOrNull` on the integer and floating types
does not resolve; `Exception.SupersededChain` is not a member; `DomainFaultedException.
FaultStackTrace` is declared but not implemented (the diagnostic cites #2337);
`Exception.ToString()` renders the type name only, where spec §31.4 requires the message when
non-empty; `Domain.Spawn` with a record or choice result emits malformed IR (p08); `GetType()`
is not implemented (the diagnostic cites #487); an owned nullable call result used as the
receiver of `?.` or `!` is never disposed, where a non-nullable one is (p19; spec K8.1
disposes an unbound owned temporary at the end of its full expression).

## 11. Open questions and what could not be done

1. **Marker shape.** MEASURED (p18): a binding arm `var f: IFailure` and a guarded arm
   `var ex: Exception when ex is IFailure` both compile, but in either the binding exposes
   only the interface's members (the guard narrows `ex` to the interface, spec §12.7), so
   `ToString()` is reachable and `Message` is not. Declaring `Message` on the interface is
   rejected on r40 for a class that inherits it from `Exception`. So the marker needs one of:
   the reporting members declared on the interface, with an inherited member accepted as the
   implementation; a compiler-known marker; or a base class `Failure: Exception` instead,
   which reads `Message` for free and costs the Parse-range condition its
   `OverflowException` identity. The bare arm `IFailure => …` and the atom need none of
   this.
2. **Unit operands.** `(try File.Delete(p))` yielding `bool` is convenient and a second result
   rule. The alternative is to reject unit operands.
3. **`(try E)` in `pure` members.** §8.1 rejects it in contract clauses; whether a pure body
   may contain one needs a sentence in spec §22.15.
4. **User exception classes.** 543 exception class declarations in the corpora derive from an
   `Exception`-family type and would be faults until they opt in. Is "unmarked means fault"
   the right default for user code? (HYPOTHESIS: yes; it is the conservative reading.)
5. **Should the query/assert pairs shrink too?** Under a `fails` clause they could; F keeps
   them on the ground that every surveyed language keeps both for collections.
6. **The lowering.** Stage 3 assumes `(try int.Parse(s))` can reach the runtime's
   non-throwing parse entry. Not prototyped.
7. **Real programs.** Every corpus here is a test corpus. The sibling lane
   `research/ergonomics-study` is gathering friction data and was not read; its findings
   should be laid against §2 before any proposal is drafted, and they are the first input to
   the gate of §8.5.
8. **A model of the atom.** bzcore-l1 is pinned at v0.345 and has no calls, values or `null`
   (§5.2). The result channel was not added to it in this lane beyond the outer-local
   fragment; a model with a failing call, the null result, the band filter and an owned
   argument is gate condition 2.
9. **The straddler rulings.** §8.1 item 5 rules three conditions and gives a default for the
   rest; the 48 type-and-section pairs were counted, not read one by one.

Could not do: measure compile-time cost of any candidate (nothing to build); count the call
sites candidate B would mark (needs type information; a lower bound is given); probe an
exception crossing the FFI boundary (needs a C callback harness; spec §32.8's abort rule is
cited instead); run the matrices or the full suite (out of a lane's scope).

## 12. Files

| Path | Content |
|---|---|
| `data/stdlib_inventory.py` → `stdlib_members.tsv`, `stdlib_summary.json`, `stdlib_tier_families.tsv`, `stdlib_exception_mentions.tsv` | the library's member declarations by tier |
| `data/tier_shapes.py` → `stdlib_tier_shapes.tsv`, `.json` | `Try` members by shape; corpus calls per member name |
| `data/stance_gap.py` → `stdlib_stance_gap.tsv`, `.json` | sections documenting an expected failure, with and without a value-shaped form |
| `data/corpus_usage.py` → `corpus_usage.tsv`, `.json`, `corpus_throw_types.tsv` | failure-signal usage in the four corpora |
| `data/doc_cost.py` → `doc_cost.json` | words, changelog revisions, proposals and decline rows spent on the tiers |
| `model/to_null_model.py`, `model/out_vars3_len3_body2.txt` | the three ownership-model checks (bzcore-l1, v0.345) |
| `probes/p01…p19*.bzl`, `probes/run.sh`, `probes/run_all.sh`, `probes/RESULTS.txt` | r40 probes and their output |

To regenerate the data (the doc-example corpus needs an exported copy):

```
mkdir -p /var/tmp/bezel-scratch/err/export
git archive HEAD docs scripts/extract_doc_examples.py | tar -x -C /var/tmp/bezel-scratch/err/export
mkdir -p /var/tmp/bezel-scratch/err/export/tests/conformance
python3 scripts/extract_doc_examples.py --repo-root /var/tmp/bezel-scratch/err/export
D=docs/research/error-model/data
python3 $D/stdlib_inventory.py . $D
python3 $D/corpus_usage.py . /var/tmp/bezel-scratch/err/export $D
python3 $D/tier_shapes.py . /var/tmp/bezel-scratch/err/export $D
python3 $D/stance_gap.py . $D
python3 $D/doc_cost.py . $D
python3 -I docs/research/error-model/model/to_null_model.py \
    docs/research/soundness/bzcore-l1/bzcore.py --vars 3 --len 3 --body 2
```
