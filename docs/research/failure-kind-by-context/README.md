# Is "failure or defect" a property of the exception's type?

*Research lane `research/failure-kind-by-context`, 2026-11-10. Non-normative: no normative
document, proposal, decline row, compiler source or test changed with it, and no issue or
proposal was filed. Pins: documents at **v0.357**, tree `e49c32aca`. No released compiler
implements the failure marker (MEASURED: no file under `src/` names `IFailure`), so no Bezel
program was run for this note. Web access was not used; prior art is from memory and is
marked so.*

*Evidence labels. **MEASURED**: produced by a script in `data/` or a program in `snippets/`,
re-runnable (§9). **QUOTED**: text of a repository document, cited. **QUOTED-from-memory**:
a published source recalled, not consulted; wording may be inexact. **HYPOTHESIS**: a
judgement or a prediction; not evidence. The scenario table of §3 is a set of judgements;
its counts are MEASURED over those judgements, and §2 says what that is worth.*

## 1. One page for the owner

**The question.** Proposal 0211 (v0.356) gives every standard-library exception class a fixed
kind: *failure* (implements `IFailure`; "a condition a correct program can meet") or *defect*
("the program is wrong"). The owner's objection: whether a missing file or a failed network
call is expected or serious depends on the use, so the library should not prescribe it;
perhaps only user exception classes should carry the distinction.

**The answer.** Three different things are being called "the kind", and they belong in three
places.

| | Who knows it | Varies with the use? (sample, §3) |
|---|---|---|
| **Origin**: could the outside world have caused this? | the raiser (the library) | Little, for the classes v0.357 marks: 161 of 172 failure-typed rows (94%) are not bugs. Much, for the classes it calls defects: only 37 of 89 rows (42%) are bugs |
| **Stance**: did *this call* expect it? | the caller | Yes. The same class is expected at one call and impossible at the next, in the same program |
| **Disposition**: carry on, report, or stop? | the handler and the program | Yes. No rule keyed on the class gets more than 53% of the sample right |

So the owner is right about what the words *failure* and *defect* suggest. What to do about
a missing file is not a property of `FileNotFoundException`. The landed text does not quite
say it is: its test is origin. But it names the two sides "failure" and "defect", teaches the
marker arm as "expected: answered" (QUOTED, Guide 2 §4), states that every unmarked exception
means "the program is wrong" (QUOTED, spec §31.4), and gives the caller no short way to
disagree. The names and the teaching are a wording problem. The statement about unmarked
exceptions is false for 52 of the 89 defect-typed rows. The missing override is a feature
that every comparable language except Java has.

**Should the library prescribe the kind of its own exception types?** It should state
*origin*, because that is a fact about the raising operation that the library knows and the
sample bears out. It should not state stance or disposition, and it should stop claiming
that an unmarked class is a bug report.

**Headline evidence.**

1. *The sample* (MEASURED over judgements: 47 tasks in 13 domains, 272 task–operation pairs,
   `data/scenarios.tsv`). Of the 17 exception types with five or more rows, 12 have no band
   (handle locally / report at a boundary / stop) holding 60% of their rows. On the owner's
   reading, a failure is something the program carries on from; 12 of 16 types sit on both
   sides. `FileNotFoundException` alone: 14 handled locally, 4 reported, 8 fatal, 1 a bug.
   The best possible rule keyed on the type predicts 53% of the rows; "who supplied the
   operand" alone predicts 70%; three call-site questions describe 92% (they are close to
   the labels' definition: §3.1). Two passes over the table agree on 268 of 272 rows, but
   the same rater made both, so that figure shows consistency, not reliability (§2).
2. *Other languages* (four samples MEASURED by compiling and running; three written from
   memory). Only Java puts the decision in the type, and its checked `IOException` is the
   standard counter-example: the JDK itself added `UncheckedIOException`. Rust, Swift and Go
   let the library set a default by origin (I/O returns an error; an index out of range
   panics) and give the caller the last word in one token (`.expect`, `try!`,
   `MustCompile`, `log.Fatal`). Python decides in the handler only. Of the seven, only
   Python has no library default, and only Java has no caller's override.
3. *Bezel's own material.* Until v0.355 the documents said the throwing form means "failure
   is a bug at this call site" (QUOTED, stdlib §1 at `156fdf9c9`); 0211 removed that gloss
   because one class can have one kind. In the 40 everyday programs, 26 of the 27 call sites
   that can raise an `IFailure` class are sites where a raise would mean a bug or a broken
   environment (MEASURED; the programs carry their data inline, which inflates this). The
   spec's and Guide 2's own examples handle `ArgumentException` and
   `InvalidOperationException` as expected conditions.

**Recommendation: amend v0.356, do not revert it.** Keep the marker and the library's
conformances as a statement of origin and a default, and add what is missing (§7):

1. **Restate** (text only). The class says who could have caused the condition. It does not
   say the program can carry on, and a class without the marker does not say the program is
   wrong. Three sentences in spec §31.4, one in stdlib §1, and Guide 2 §4 change.
2. **Give the caller the last word** in the direction Bezel lacks: "a failure here is a
   defect". Today that is a catch-and-wrap of five lines, or `ParseOrNull(…)!` where a
   query twin exists. Design it together with the deferred `(try E)` atom, which is the other
   stance.
3. **Give data-caused "defect" conditions a query form.** Arithmetic is the clear case: 10
   of 13 overflow rows are expected results of the data being computed, 2 are bugs and 1 is
   a test oracle. This is the reopening bar that the decline row for 0211 Q7 already names.

**The alternative, and when it is the right choice.** If the owner wants the marker to mean
"this application expects it", then only the application can say so, and option B (no
library class is marked) is the coherent design. It is cheap now: nothing is implemented,
and at most 30 of the 5,970 rule-corpus cases change. Its costs are that a tool's top-level
arm must list the library classes by name, that a third-party library has the same problem
one level out, and that the deferred `(try E)` atom loses its definition (§6).

**One rule for the user**, if the recommendation is taken:

> The class says who could have caused it. The call says whether you expected it. The
> handler says what happens next.

## 2. Method, and what it is worth

**The sample.** 47 tasks across CLI tools, web services, batch jobs, desktop applications,
libraries, start-up and configuration, database access, background workers, tests, embedded
devices, games, compilers and a plugin host. For each task, the operations that can go
wrong: 272 pairs. Each pair has one line saying what a well-written program does, and a
label:

| Label | Meaning | The brief's band |
|---|---|---|
| **L** | handle it at the call or in its loop; the same piece of work continues | (a) |
| **B** | drop that piece of work, report or retry it at a boundary; the program continues (a one-shot tool exits with a message for its user) | (b) |
| **F** | fatal environment or configuration: stop, tell the operator; the code is right | (c) |
| **D** | a bug: only a mistake in the program can cause it | (c) |

The brief's band (c) is split because "stop" covers two things: the owner's "major defect if
the database file is missing" is F, not D, and v0.357's test separates them.

Each pair also records the condition (35 conditions, `data/conditions.tsv`), mapped to the
class a v0.357 library raises for it and that class's kind, and three facts about the call:
who supplied the operand (user, peer, environment, operator, the program itself, hosted
code, or unknown to a library), whether the task has a fallback, and whether one unit of
work or the whole program is affected.

**Two readings are scored.** *The owner's reading*: a failure is something the program
carries on from (L, B); a defect is something it stops for (F, D). *The letter of v0.357*: a
failure is a condition a correct program can meet (L, B, F); a defect is a bug (D).

**The contextuality index.** For a type, `flip` is one minus the share of its largest band
(a, b, c). A type is *stable* at `flip` ≤ 0.15, *leaning* up to 0.40, *contextual* above.
The thresholds are arbitrary; the tables give the counts.

**Limits. Read these before the numbers.**

- **One rater, and the second pass is not independent.** The second pass was made later in
  the same session, from a shuffled sheet showing only the task and the operation
  (`data/pass2_sheet.py`), against a restated question. The rater was the same model with
  the first pass in its context. Agreement of 268/272 (κ = 0.98) therefore measures
  consistency. An independent rater should relabel the table before a proposal cites its
  rates (§8).
- **The sample is designed, not drawn.** The share of tasks that are hosts, start-up code or
  tools sets every rate. Rates are not estimates of any population of programs. What does
  not depend on the mix: each flip is a concrete pair of rows, one class, two tasks, two
  right answers.
- **Two label boundaries are soft.** L against B depends on what counts as the unit of
  work. F against D depends on whether a shipped file counts as code; three of the four
  disagreements are there.
- **A most-favourable reading is reported beside the raw one** (§3.4): it drops every row a
  defender of the landed text could explain away, by rule, without re-judging anything.
- **Class mapping.** Network conditions have no landed class (proposal 0001 is under
  review); they are scored as failures by 0211's origin test. A condition with a query form
  (a missing key, a bad index) raises nothing in a program that takes the query form; the
  mapping says what the throwing form would raise.

## 3. The scenario sample

### 3.1 Three readings of the same table

MEASURED (`data/contextuality.py` → `data/results.json`). 261 of the 272 rows have a class
with a kind; 11 do not (cancellation 4, allocation failure 5, the domain join 2).

| | Failure-typed rows (172) | Defect-typed rows (89) |
|---|---|---|
| L: handled at the call | 83 | 35 |
| B: reported at a boundary | 44 | 11 |
| F: fatal here, code is right | 34 | 6 |
| D: a bug | 11 | 37 |
| **fits the owner's reading** | 127 (74%) carry on | 43 (48%) stop |
| **fits the letter** | 161 (94%) are not bugs | 37 (42%) are bugs |

Over all 261 rows the kind fits the owner's reading in 170 (65%) and the letter in 198
(76%); the second pass gives 171 and 199.

How much of the three-band label does each fact predict? Each figure is the share of rows a
rule keyed on that fact alone would get right if it always chose the commonest band for
each value: an upper bound for any such rule.

| Rule keyed on | Rows right |
|---|---|
| the kind (failure / defect) | 49% |
| the exception type | 53% |
| the condition (35 values) | 55% |
| the task | 70% |
| who supplied the operand | 70% |
| three call-site questions: is there a fallback; who supplied the operand; does one unit or the whole program stop | 92% (four labels) |

The last row is close to circular: the three facts were written down in the same pass as
the labels, and "there is a fallback" is nearly the definition of L. It is a description of
what the labels rest on. The point of the table is the first three rows.

### 3.2 Per type

MEASURED (`data/contextuality_by_type.tsv`; `data/contextuality_by_condition.tsv` has the
same by condition). Types with five or more rows.

| Type | v0.357 kind | Rows / tasks | L | B | F | D | Carries on | A bug | `flip` | Verdict |
|---|---|---|---|---|---|---|---|---|---|---|
| `SerializationException` | failure | 32 / 27 | 15 | 8 | 6 | 3 | 72% | 9% | 0.53 | contextual |
| `FileNotFoundException` | failure | 27 / 22 | 14 | 4 | 8 | 1 | 67% | 4% | 0.48 | contextual |
| `FormatException` | failure | 26 / 21 | 12 | 4 | 5 | 5 | 62% | 19% | 0.54 | contextual |
| `KeyNotFoundException` | defect | 26 / 22 | 8 | 6 | 5 | 7 | 54% | 27% | 0.54 | contextual |
| `IOException` (permission, disk full, other) | failure | 25 / 18 | 9 | 9 | 7 | 0 | 72% | 0% | 0.64 | contextual |
| network (not landed) | failure by origin | 24 / 17 | 9 | 10 | 5 | 0 | 79% | 0% | 0.58 | contextual |
| `IndexOutOfRangeException` | defect | 13 / 13 | 6 | 0 | 0 | 7 | 46% | 54% | 0.46 | contextual |
| `OverflowException` | defect | 13 / 13 | 6 | 5 | 0 | 2 | 85% | 15% | 0.54 | contextual |
| `TimeoutException` | failure | 12 / 10 | 6 | 5 | 0 | 1 | 92% | 8% | 0.50 | contextual |
| `InvalidOperationException` | defect | 10 / 8 | 1 | 0 | 0 | 9 | 10% | 90% | 0.10 | **stable** (a bug) |
| `ArgumentException` | defect | 8 / 8 | 4 | 0 | 1 | 3 | 50% | 38% | 0.50 | contextual |
| `ParseRangeException` | failure | 8 / 8 | 5 | 2 | 1 | 0 | 88% | 0% | 0.38 | leaning |
| `NullReferenceException` | defect | 7 / 7 | 3 | 0 | 0 | 4 | 43% | 57% | 0.43 | contextual; stable (a bug) outside hosts |
| `EndOfStreamException` | failure | 6 / 6 | 3 | 1 | 2 | 0 | 67% | 0% | 0.50 | contextual |
| allocation failure | fatal | 5 / 5 | 2 | 0 | 3 | 0 | 40% | 0% | 0.40 | leaning |
| `ContractViolationException` | defect | 5 / 5 | 2 | 0 | 0 | 3 | 40% | 60% | 0.40 | leaning; stable (a bug) outside hosts |
| `DivideByZeroException` | defect | 5 / 5 | 5 | 0 | 0 | 0 | 100% | 0% | 0.00 | **stable, against its kind** |

Both passes give the same verdict for every type.

- **Which types flip.** On the three bands, 12 of 17. On the owner's reading (carries on
  against stops), 12 of the 16 types with a kind have at least 15% of their rows on each
  side; the four that do not are `TimeoutException` and `ParseRangeException` (carry on),
  `InvalidOperationException` (stops) and `DivideByZeroException` (carries on, though it is
  a defect class).
- **Which are stable on the letter** (is it a bug?). The seven failure types other than
  `FormatException` are a bug in 0% to 9% of their rows: the marker's statement of origin
  holds (the network row is scored by origin; it has no landed class).
  `InvalidOperationException` is a bug in 90%. Six other defect classes are a bug in 15% to
  60% of their rows, and `DivideByZeroException` in none.
- **Hosts.** A test runner, a test asserting a rejection, an interpreter and a plugin host
  (15 rows) treat every exception from the code they host as an expected event. That is
  where `NullReferenceException` and `ContractViolationException` flip, and nowhere else.
  v0.357 handles hosts with the catch-all arm, so they do not count against it.

### 3.3 The owner's two examples

MEASURED from `data/scenarios.tsv`.

- **A missing file**: 27 rows in 22 tasks. Handled at the call in 14 (a file the user chose
  in an editor, T15a; an optional settings file, T26a; a first run, T17a, T30a). Reported at
  a boundary in 4 (a source file named on a compiler's command line, T44a). Fatal in 8 (the
  required database file, T25a; a bundled asset, T41a; a signing key, T09d). A bug in 1 (a
  test fixture that ships with the code, T37a).
- **A refused connection**: 11 rows. Handled at the call in 4 (optional telemetry, T07a; a
  cache server, T34b). Reported at a boundary in 4 (the upstream a gateway exists to call,
  T06a). Fatal in 3 (the database at start-up, T25c; the secrets store, T27a).

In every one of these rows the class and the condition are the same. What differs is at the
call: who named the file, whether there is a default, and whether anything else can proceed.

### 3.4 Where the v0.357 assignment is wrong, by kind of mismatch

MEASURED (`results.json`, `mismatch_classes`, with the row ids).

| Mismatch | Rows | What v0.357 does with it |
|---|---|---|
| **A failure class for a bug**: parsing a literal or a constant (T16c, T23d, T41c), a shipped file or document (T16b, T37a, T37b), a migration's wrong assumption (T14a), a channel the same code closed (T33c), a loop missing its own deadline (T38g), and two of 0211's recorded misfits (a format specifier, T13f; an unregistered type, T20f) | 11 | An `IFailure` arm above the call answers a bug as an expected failure |
| **A failure class for a condition that is fatal here** | 34 | Right by the letter. 36 of the 45 fatal-or-bug rows are raised at start-up or at the top of a one-shot tool, where no arm is above them and the program ends with its report. 9 are raised beneath a unit-of-work boundary (§3.6) |
| **A defect class for an expected condition, query form exists** (a key, an index, an empty collection, a duplicate key, a null) | 25, 5 of them in hosts | Nothing, if the caller takes the query form. If the caller uses the throwing form from habit, as Python, Java and C# programmers do, the boundary arm passes it as a bug |
| **… a tester only** (division: test the divisor) | 5, 1 in a host | The same |
| **… no query form**: arithmetic whose operands are data (10: T04c, T05d, T09c, T11g, T13c, T18c, T23c, T38d, T42d, T45b), a date component the user typed (T23a), and five host rows | 16 | The caller validates ranges before computing, or names a defect class in a handler |
| **A defect class for a misconfiguration** (a required key absent from a parsed configuration, five rows; a pool size of zero) | 6 | Ends the program with a bug report for an operator's mistake, unless the caller took the query form |

**The most favourable reading.** Drop the 15 host rows. Drop every defect-typed row whose
condition has a query form or a tester, on the ground that a well-written Bezel program takes
it. Score the rest on the letter. 217 rows remain and 23 (11%) do not fit: the 11 bugs
raised with a failure class, and 12 data-caused conditions raised with a defect class that
has no query form (10 overflows, the date component, a pool size of zero from
configuration). These cannot be removed by reading the rows more kindly. They are the two
gaps the recommendation addresses.

### 3.5 What decides

The three facts recorded per row, against the labels (MEASURED, first pass):

| Who supplied the operand | L | B | F | D |
|---|---|---|---|---|
| the user or the data being processed (72) | 46 | 23 | 1 | 2 |
| a peer (33) | 15 | 17 | 1 | 0 |
| the environment (57) | 34 | 12 | 11 | 0 |
| the operator: configuration, installation (33) | 4 | 0 | 29 | 0 |
| the program: constants, own state, arithmetic (55) | 7 | 2 | 1 | 45 |
| hosted code (15) | 14 | 1 | 0 | 0 |
| unknown to a library (7) | 0 | 5 | 0 | 2 |

None of these facts is visible to the class. All are visible at the call. A library sees the
last row: it cannot tell a URL literal from a URL the user typed (T21d), which is why Go
ships `regexp.Compile` and `regexp.MustCompile` for one condition.

### 3.6 A marker arm at the unit-of-work boundary

MEASURED (`results.json`, `marker_arm_at_the_unit_boundary`; the placement of the 45
fatal-or-bug rows is a hand annotation, `data/failure_typed_stop_rows.tsv`).

- Of the 55 rows a boundary should answer (label B, with a kind), an `IFailure` arm takes 44
  (80%). It passes 11 as bugs: six lookups by a key the user or the data supplied, which
  have a query form, and five overflows of external magnitudes, which do not.
- Of the 45 failure-typed rows that are fatal or a bug, 9 are raised beneath a unit-of-work
  boundary, where the arm answers them as routine: disk full inside the per-file loop of a
  backup tool whose handler skips unreadable files (T03b); an expired certificate inside the
  retry loop that backs off on a refused connection (T40e); database corruption inside the
  transaction boundary that retries a lock time-out (T30b, T30c); a schema mismatch inside a
  request (T28e); the loop's own missed deadline beside a bus time-out (T38g); a channel the
  stage itself closed (T33c); a literal parsed in a form handler (T23d); an unregistered
  type (T20f).

What could separate each of the nine from the routine failures beside it (the
`separable_by` column of the same file):

- **3 have a class of their own** (T30b, T30c, T40e): an arm on that class, written before
  the marker arm, separates them in v0.357.
- **4 share a class with a routine condition raised at a different call** (T20f, T23d, T28e,
  T38g): only a conversion at the call separates them. This is part 2 of the recommendation.
- **2 share a class and a call** (T03b: the same copy raises `IOException` for an unreadable
  file and for a full disk; T33c): no call-site form can separate them. The handler needs
  the cause as a value. That is a property of catching by class, not of the marker: an
  `IOException` arm has the same fault (§8, question 5).

## 4. Other languages

### 4.1 The same four situations in seven languages

`snippets/` holds one sample per language of four situations, each in an expected and a
must-not-happen form: a file the user chose against a file the program cannot run without;
optional telemetry against the upstream the service exists to call; a number the user typed
against a constant; a key the user supplied against a key the program put there.

MEASURED (`snippets/run.sh` → `snippets/RESULTS.txt`; Go 1.24, Rust 1.97, JDK 21, Python
3.13): the Go, Rust, Java and Python samples compile, and in each the expected forms print
their message and exit 0, while the must-not-happen forms end the process (exit 1 or 2, 101
for a Rust panic). The C#, Swift and Kotlin samples were **not compiled** (no toolchain on
the machine) and are from memory.

| Language | The library's default | The caller says "this is a bug here" | The caller says "I expect this" | Who decides |
|---|---|---|---|---|
| **Java** | the class: checked (`IOException`) or unchecked (`NumberFormatException`) | a handler that wraps: `catch (IOException e) { throw new UncheckedIOException(e); }` | `try`/`catch` | **the type**. MEASURED: a caller with no handler for the missing required file is rejected, "unreported exception IOException; must be caught or declared to be thrown" (`CheckedForcesHandler.java`) |
| **Rust** | the API's shape: `Result` where the outside can defeat the call, a panic where only the caller can (`v[i]`, `map[k]`) | `.expect("…")`, `.unwrap()` | `match`, `?`, and `get` / `checked_add` beside the panicking forms | the library sets a default by origin; **the call** has the last word in one token, both ways |
| **Swift** | `throws` against a trap | `try!` | `try?`, `do`/`catch`; `Int("x")` and `dict[k]` return optionals | the same |
| **Go** | `error` against `panic` | `log.Fatal(err)`, `panic(err)`, and a second entry point: `regexp.MustCompile` beside `regexp.Compile` | `if err != nil`, `errors.Is(err, fs.ErrNotExist)`, `v, ok := m[k]` | the same |
| **Kotlin** | Java's classes without the checked rule; `toInt` / `toIntOrNull`, `getValue` / `get` | the throwing twin, or no handler | the `OrNull` twin, or `try`/`catch` | the call, by which twin it picks |
| **C#** | guidelines sort classes into usage errors and execution errors; `Parse` / `TryParse`, the indexer / `TryGetValue` | the throwing twin, or no handler | the `Try` twin, or `try`/`catch` | the call, by which twin it picks; the handler, by the classes it names |
| **Python** | none: one hierarchy arranged by subject | no handler (the traceback is the report), `assert`, `sys.exit` | `except KeyError:` at the call is the idiom | **the handler** |

### 4.2 The history, and what it says

All QUOTED-from-memory unless marked; wording may be inexact and none was checked against a
source.

- **Java's checked exceptions.** The tutorial's rule is "If a client can reasonably be
  expected to recover from an exception, make it a checked exception. If a client cannot do
  anything to recover from the exception, make it an unchecked exception." The rule defines
  the kind by what *the client* can do and then attaches it to *the class*, which has every
  client. `IOException` is the usual example of the result: the caller for whom a missing
  file is fatal must still write a handler. Java 8 added `java.io.UncheckedIOException` to
  the JDK so that `Files.lines` and stream lambdas could raise I/O errors at all (the class
  exists and is used in `snippets/Demo.java`: MEASURED). Spring translates the checked
  `SQLException` into an unchecked hierarchy. `NumberFormatException`, the plainest input
  failure, is unchecked. C#, Kotlin and Scala declined the feature; Hejlsberg's stated
  reasons were versioning and scale. *Effective Java* keeps the rule ("checked exceptions
  for recoverable conditions") and immediately qualifies it ("avoid unnecessary use of
  checked exceptions").
- **.NET.** The *Framework Design Guidelines* separate usage errors (fix the calling code;
  never catch) from execution errors, and recommend the Tester-Doer and Try-Parse patterns
  for members that fail in common scenarios. Lippert's "vexing exceptions" sorts exceptions
  into fatal, boneheaded (your bug), vexing and exogenous (the outside: you must handle it).
  That is a classification by type and by origin, close to 0211's; its *vexing* class,
  with `Int32.Parse` as the example, is the admission that one condition is expected at
  some calls, and the answer was a second entry point. Older .NET raised `ArgumentException`
  for an illegal path the user typed; later versions stopped pre-validating paths. Bezel
  raises `IOException` there (QUOTED sense, stdlib §7.2: "every other OS-level open
  failure"), which the sample scores as right (T08d).
- **Rust.** *The Rust Programming Language*, "To `panic!` or Not to `panic!`": returning a
  `Result` gives the calling code options; the caller may decide that an `Err` is
  unrecoverable in its case and turn it into a panic; so `Result` is "a good default choice"
  for a function that might fail. And: when "you have more information than the compiler",
  as with a hard-coded IP address, `expect` is right.
- **Swift.** *The Swift Programming Language*, "Disabling Error Propagation": `try!` is for
  a call you know cannot throw at run time, and the book's example is loading an image that
  ships with the application. That is the owner's database-file example, in the vendor's
  own book.
- **Go.** `regexp.MustCompile` "is like Compile but panics if the expression cannot be
  parsed. It simplifies safe initialization of global variables". *Effective Go* allows a
  panic where a library "truly cannot set itself up".
- **Midori** (Duffy, "The Error Model"). Bugs abandon the process; recoverable errors are
  declared and marked with `try` at the call. I/O and the validation of outside data are on
  the recoverable side; a null dereference, an index out of range, overflow, a broken
  contract and allocation failure are on the bug side. Allocation failure had an opt-in
  recoverable form for callers that could do something about a large allocation (low
  confidence in this detail). A parent
  process handled a child's abandonment: one component's bug is its supervisor's expected
  event.
- **P0709** (Sutter). The same two bands: bugs go to contracts, recoverable errors are
  thrown as values, heap exhaustion fails fast by default with an opt-in reporting form.
- **Web frameworks** (ASP.NET Core, Spring MVC, Django, Rails). The request boundary maps
  exception classes that the application or the framework declares (`Http404`,
  `ResponseStatusException`, `RecordNotFound`) to client errors and maps everything else,
  I/O exceptions included, to a 500 logged as an error. At that boundary the expected set
  is declared by the application, not by the standard library. **This supports the owner's
  suggestion** for services.

### 4.3 The pattern

1. **By type only**: Java. The design is widely regarded as a mistake on exactly this
   point, and its own standard library works around it.
2. **A library default by origin, with the caller's last word at the call**: Rust, Swift,
   Go, Midori; Kotlin and C# approximate it with twin methods. The default is 0211's origin
   test. What these languages add is a one-token conversion in each direction. Even
   allocation failure, which P0709 and (as recalled) Midori treat as fatal, got a
   caller-chosen form; in the sample it is handled in 2 of 5 rows (an editor refusing a
   4 GB file).
3. **By handler only**: Python; and, at framework boundaries, the exception languages in
   practice.

Two things follow. Only Python among the seven has no library default; the owner's
proposal would put Bezel with Python, and with what the web frameworks do at their boundary.
And only Java lacks the caller's override; v0.357 lacks half of it. One difference from
pattern 2
matters: in Rust, Swift and Go an unconverted library error is not yet *expected*, it is
*undecided*, and the type system makes the caller decide. Bezel's exceptions propagate
without a mark at the call, so its default applies without anyone having decided. That makes
the wording of the default matter more in Bezel than in those languages.

## 5. Bezel's own evidence

### 5.1 The doctrine before v0.356

MEASURED (`git show 156fdf9c9:docs/Bezel_Standard_Library.md`, the v0.355 tree): stdlib §1
called the throwing tier "the assert-shaped throwing access ("failure is a bug at this call
site")", said the throwing form "treats absence as a bug", and stdlib §2.1 said "end of
input is a bug at its call sites". Guide 6 §2 called `int.Parse` on boundary input a
re-assertion of "the same misclassification the bad `requires` made". That is the owner's
position: the kind is the caller's, chosen by the form it calls.

0211 rewrote these glosses (its Q6), for a stated reason: "one class has one kind", and
`FormatException` is both what `int.Parse` raises and what Guide 6's `Listen` throws
(QUOTED, 0211 §4.1). It records the change as "a consequence of one class having one kind,
not a goal" (QUOTED, 0211 §14). The v0.357 text keeps the stance ("this call site handles
nothing: it asserts the operation succeeds") and adds that the stance "does not make the
exception a failure or a defect" (QUOTED, stdlib §1). So v0.357 has a caller's stance that
no handler can see.

### 5.2 The 40 everyday programs

MEASURED (`data/everyday_sites.py` over `docs/research/ergonomics-study/programs/*/final.bzl`;
the operand's origin and what a raise would mean are hand annotations,
`data/everyday_sites_annot.tsv`).

| | Sites |
|---|---|
| call sites where a condition of §3 can arise | 43 |
| throwing forms that raise an `IFailure` class: `Parse` 5, file and directory calls 18, deserialization 4 | 27 |
| … where a raise would mean a bug or a broken environment: a literal 2, an embedded constant 4, the program's own output 2, a file the program itself wrote 18 | **26** |
| … where a raise is expected: `int.Parse(text)` in a `try`, converted to the program's `ConfigError` | 1 |
| `Parse` sites on input the program expects to be bad, all three using `TryParse` | 3 |
| lookups using a query form (`GetOrNull`) against throwing state forms (`First()`, `Pop()`, `Average`) | 8 against 4 |
| throwing sites outside any `try` | 30 of 31 |

The programs already choose the stance at the call, by choosing the form: `TryParse` where
the text may be bad, `Parse` where it cannot be. Under v0.357 the 26 assert-stance sites
raise *failures*. At the top of a one-shot program that is harmless. Beneath a boundary's
`IFailure` arm it would answer a bug as bad input.

Limits: the study's tasks carry their data inline, so "own data" sites are over-represented;
a program reading real input would have more expected sites. The count shows that both
stances occur on the same member, not their ratio in the field.

Three more facts from the same programs (MEASURED, 0211's `catch_arms.py` and a read of the
sources). They contain 8 catch arms. Two catch `InvalidOperationException`, a defect class,
which the program itself threw on purpose (a currency mismatch in t07, a deliberate fault
in t36). The calculator (t35) tests the divisor and throws its own
`ParseError("division by zero")`: the author turned a defect condition into an expected one
by hand. None of the 40 has a catch-all; each of the 8 arms names one class.

### 5.3 The corpora

MEASURED (`data/corpus_arms.py` over the per-arm census of 0211's `catch_arms.py`; four
corpora, 3,394 arms).

| Arms naming | Type mentions | Handling completes in the arm |
|---|---|---|
| a defect class | 1,751 | 1,727 |
| a failure class | 454 | 453 |
| `Exception` alone | 625 | 576 |
| a user class or interface | 530 | 506 |
| `IFailure` | 36 | 34 |
| `OperationCanceledException` | 32 | 32 |

79% of the arms that name a classified library class name a *defect* class, and 99% of
those complete handling; the rest wrap (11) or re-raise (13). These are test programs
and the arms are oracles, so this says nothing about what applications expect. It does say
that "a defect surface is never a recovery surface" (QUOTED, stdlib §1) describes a norm,
not the corpus, and it is the population of T36 in the sample.

**The documents' own examples** (the 31 arms of the document-example corpus) have six arms
that name a defect class and complete handling. Three are in examples about something else
(an out parameter, disposal order). Three present the handling as the point. Spec §31.2.2:
`var ex: ArgumentException when
ex.ParamName = "amount" => Console.WriteLine("Invalid amount")` and
`InvalidOperationException => Console.WriteLine("Invalid state")`. Guide 2 §4: `var e:
InvalidOperationException => Log($"bad state: {e.Message}")`. QUOTED, v0.357.

### 5.4 The strain 0211 records on itself

QUOTED from proposal 0211 and stdlib §1 at v0.357:

- Three classes were raised in both bands (`OverflowException`, `FormatException`,
  `SerializationException`); two bridging classes were added so that one condition could
  change kind without leaving its class (`ParseRangeException`, `DomainFailedException`).
- Six conditions are "a recorded misfit": a band-1 condition raised with a failure class.
  "A kind is inherited and cannot be shed", so each can be fixed only by breaking every arm
  that names the class, and "this landing was the cheapest moment there will be".
- The ruling on arithmetic over external data is "the weakest ruling of the set"; the
  library "offers no query" for it.
- Adding `IFailure` to a class is a **breaking** change (Appendix E).
- The taxonomy table classifies 116 *conditions*; the kind was then pushed onto 31 classes.

HYPOTHESIS: each item is the same observation as §3. The class is a coarse carrier even
for origin, and the two bridges and six misfits are the cases where the library itself is
the caller that knows better.

### 5.5 Which of 0211's assignments are wrong, where

| 0211's assignment | Wrong in the sample when | Rows |
|---|---|---|
| `FormatException` is a failure | the text is a literal, a constant or a shipped resource | T16c, T23d, T41c, T14a; and the specifier misfit T13f |
| `FileNotFoundException`, `SerializationException` are failures | the file or document ships with the program or was just written by it | T37a, T37b, T16b |
| `ChannelClosedException` is a failure "whichever party closed the channel" (0211 Q8 b) | the sending code closed it itself | T33c; the same class is the normal end of a stream in T33a |
| `TimeoutException` is a failure | the deadline is the program's own loop budget | T38g |
| `OverflowException` on external data is a defect (0211 Q7) | the magnitudes are the user's, a peer's or a sensor's | 10 rows; an attacker chooses them in T09c and T18c |
| `ArgumentOutOfRangeException` from a `DateTime` constructor is a defect | the user typed the components | T23a |
| `KeyNotFoundException`, `IndexOutOfRangeException`, `InvalidOperationException` on an empty collection are defects | the key, the index or the data is the user's, a peer's or a host's guest's | 21 rows; all avoidable with the query form |
| `DivideByZeroException` is a defect | the divisor is data | 5 of 5 rows |
| a failure is "expected: answered" at a boundary (Guide 2 §4) | the condition is fatal for the deployment and is raised beneath that boundary | T03b, T28e, T30b, T30c, T40e |
| write-side `SerializationException` is the program's own doing (0211 §4.4) | the unrepresentable value came from a sensor | T40f |

## 6. Design options

Costs are MEASURED by `data/landing_reach.py`: 58 lines of the normative documents name
`IFailure`, 17 name `ParseRangeException`, 11 `DomainFailedException`. 44 of the 5,970
rule-corpus cases name `IFailure`: 18 rely on a library class being a failure, 18 use only
classes the case declares, 8 name the marker without either; 12 cases name
`ParseRangeException` and 5 `DomainFailedException`. No compiler source names the marker.

### A. Keep v0.357 as landed

- **Changes.** None.
- **Carries.** Two bridging classes, six flagged misfits, a kind that cannot be shed, and a
  classification duty for every future library exception (0211 §14 counts 24 classes in
  unlanded proposals, 10 of which must state a kind).
- **The owner's examples.** The user's file: an arm on `FileNotFoundException` at the call.
  The database file at start-up: no arm; the program ends with the uncaught-exception
  report. The same file opened lazily beneath a request boundary: the `IFailure` arm answers
  it as routine. Telemetry: `IFailure => ()` around the send, which is better than a list
  because a bug in the telemetry code still propagates. The primary API: propagates to the
  boundary.
- **Verdict.** The mechanism works. The text over-claims (§1), and the 23 rows of the most
  favourable reading have no good answer.

### B. No library kind; `IFailure` for user classes (the owner's suggestion)

- **Changes.** Spec §31.4: `FormatException` loses the marker; the two-kinds sentence
  becomes "a class may be declared a failure by its author". Stdlib §1: band 4 no longer
  says "whose class implements `IFailure`"; the *Failures and defects* paragraph (the origin
  test, the list of failure types, the misfits) goes or becomes guidance; §2.15, §6.1, §6.9
  and §7.5 lose the marker. `ParseRangeException` is removed, since the kind was its only
  reason, and four conditions go back to `OverflowException`. `DomainFailedException` stays,
  because a user failure can still leave a domain. The second conformance restriction
  shrinks to the cancellation case. The catch-binding rule and the domain-join rule stay.
  The three glosses of the throwing tier can return to "a bug at this call site".
- **Cost.** At most 30 rule cases deleted or converted (18 and 12, which may overlap); 18
  stay. Trackers #8414 (the library conformances) and #8417 (`ParseRangeException`) close
  unimplemented.
- **Coherence.** Sound. One loss: the deferred `(try E)` atom is defined on the marker
  ("raises an exception whose class implements the marker", QUOTED, note §8.1), so
  `(try int.Parse(s))` would no longer yield `null`, and the atom could no longer let the
  library state each operation once.
- **The rule the user learns.** "The library does not say which of its exceptions you
  should expect. Catch them by name where you expect them, or wrap them in a class of your
  own that lists `IFailure`."
- **The owner's examples.** The same as under A at both call sites. The difference is the
  boundary arm, which takes neither file error unless the program wrapped it. A tool's
  top-level arm lists `IOException | FormatException | SerializationException`, or a missing
  input file ends the tool with a bug report.
- **The gap.** Libraries. A third-party HTTP client that marks its connect exception
  prescribes exactly what the standard library would no longer prescribe. The consistent
  form of B is "only the application marks", and then the marker need not be built in.
- **Verdict.** Right if the marker is to mean "this application expects it". It matches
  what web frameworks do (§4.2). It gives up the tool's one-arm boundary and the `(try E)`
  path.

### C. A default on the class, and the caller's last word

- **Changes.** (1) Restate the marker as origin and as a default (text only). (2) Add a
  call-site conversion from failure to defect that keeps the cause. (3) Give data-caused
  conditions on the defect side a query form, starting with checked arithmetic.
- **Cost.** Additive: no existing rule case changes. One expression form or library member,
  with its cases; guide edits.
- **Coherence.** The kind is still read in two places only (an arm, a join). The conversion
  raises an ordinary defect, so §30.14 carries it as one. It fits the deferred `(try E)`:
  the two are the caller's two stances, and Swift's `try?` and `try!` are the precedent.
  Contracts are untouched.
- **The rule the user learns.** The three sentences of §1.
- **The owner's examples.** The user's file: an arm at the call. The database file: nothing
  at start-up; where a boundary is above the open, the open is written with the conversion
  and passes the boundary as a defect whose cause is the missing file. Telemetry:
  `IFailure => ()`. The primary API: propagates.
- **Verdict.** Recommended.

### D. Classification by the handler only; no marker

- **Changes.** Revert the marker: spec §2.3's row, §31.4's paragraph, §30.14's join
  sentence, Appendix C.7 and E; stdlib §1 back to v0.355's bands; both classes removed.
- **Cost.** The largest: all 44 cases, five trackers, most of 0211's 63 edits.
- **Coherence.** Sound; it is v0.355. It brings back what 0211 measured: a boundary has no
  arm that lets a bug through, and the corpora had 566 undivided catch-alls.
- **Verdict.** Honest and simple. It discards the one thing v0.356 added that the sample
  supports (the marked classes are not bugs in 94% of rows), and replaces it with a guideline.

### E. The kind at the raise site

- **E1, a kind on the instance, set by the `throw`.** It would dissolve both bridging
  classes and the six misfits, because a condition and not a class would carry the kind.
  It needs a catch selector that is not a type, with new rules for arm ordering and
  unreachable arms; 0211 declined it for that reason. The sample adds a second reason: of
  the 11 bugs raised with a failure class, only 2 are conditions the raising member can
  recognise (T13f, T20f). The other 9 are known only to the caller.
- **E2, a per-member contract in the documents only** (the condition table made normative,
  no marker). No arm can select on a table: this is D with more text.
- **Verdict.** E1 fixes the smaller part of the problem at the larger price. Not
  recommended.

### Summary

| | A keep | B user classes only | C default + caller | D handler only | E1 raise site |
|---|---|---|---|---|---|
| Normative text changed | none | spec §31.4; stdlib §1 and six sections | three sentences, plus one new form | most of v0.356 | catch dispatch |
| Rule cases changed | 0 | ≤ 30 | 0 (new cases added) | 44 | all 44, plus grammar |
| Tool's one-arm boundary | yes | no (a list) | yes | no | yes |
| Bug raised with a failure class (11 rows) | answered as routine | passes as a bug | passes, where the call says so | by name | 2 of 11 fixed |
| Data-caused defect, no query form (12 rows) | passes as a bug | by name | query form | by name | unchanged |
| Deferred `(try E)` | compatible | loses its definition | compatible; its sibling | not definable | compatible |
| Library authors | classify by origin | should not classify | classify by origin | nothing to do | classify per throw |

## 7. Recommendation

**C, as an amendment of v0.356.** Reasons, in order of weight:

1. **Origin is the part that is stable, and v0.357's marking gets it right**: 94% of
   failure-typed rows are not bugs (§3.1). Removing the marking (B, D) throws away a fact
   the library knows and every boundary would otherwise re-declare.
2. **Stance and disposition are not the library's to state**, and v0.357 states them in
   three places. Each can be fixed without removing anything.
3. **Every comparable design has the caller's last word**, and the sample's residue (23
   rows on the most favourable reading; 26 of 27 sites in the everyday programs) is
   exactly the two conversions Bezel lacks.
4. **It is additive from v0.357** and keeps the deferred `(try E)` path open; B closes it.

**Should v0.356 be reverted in part?** No part of its mechanism. Its wording should change
in three places, one ruling's side effect needs a remedy, and one decline row's reopening
bar names a remedy this note recommends:

| v0.356 text or ruling | Recommended |
|---|---|
| Spec §31.4: "the kind is a property of its class"; "Every other exception is a **defect** report: the program is wrong" | Amend: the class states origin; an unmarked class makes no statement |
| Stdlib §1, band 1: "A defect surface is never a recovery surface" | Keep for `requires`. Narrow for the residual classes: a caller that expects the condition takes the query form, and where none exists the library owes one |
| Guide 2 §4: "its class says which"; the arm comment "expected: answered"; "Wrap a failure in a failure and never wrap a defect" | Amend, and add the third habit: a failure that cannot be the outside's doing at this call is converted at the call |
| 0211 Q6 (the kind is the condition's, not the tier's) | Stands. Its side effect, that the idiomatic assert spelling `int.Parse("8080")` raises a failure, gets a remedy (part 2) |
| 0211 Q7 (arithmetic on external data is a defect; no query surface) | The classification stands. Its reopening bar names a query surface as what would change the premise: propose that surface (part 3) |
| 0211 Q8, Q9, Q10, Q12 and the two bridging classes | Stand |

**If the owner prefers B**, this is the cheapest moment for it, for the reason 0211 gave for
its own misfits: nothing is implemented and nothing outside the repository depends on the
marking. The outline below then changes in its section 3 only.

**What would change the recommendation** (HYPOTHESIS). Programs written against a compiler
that has the marker, which is also proposal 0213's trigger. If their boundaries rarely put
an `IFailure` arm over library calls, or if such arms often answer a fatal condition or a
bug as routine, the library marking is not earning its taxonomy and B is the better design.
If the arms are used and the mis-answers are the kind part 2 removes, C stands.

### 7.1 Proposal outline (not filed)

In the shape `docs/proposals/spec-change-process.md` §6 expects. Everything marked ▲ is
proposed and exists in no document.

| | |
|---|---|
| **Status** | (would be) under-review — draft |
| **Tracking issue** | to be opened at filing |
| **Target documents** | **spec** §31.4 (*The failure marker*: three sentences), §31.2.5 (one sentence), §31 (the conversion, if it is an expression form), Appendix A (only then). **stdlib** §1 (*Failures and defects*: one sentence; band 1: one clause), §9.1 (the checked-arithmetic members, if in scope). **rationale** §11.12 (one paragraph). **Guide 2** §4 and §7; **Guide 6** §2; the **code-generation reference** §5 and §6. **proposals** `README.md`, `DECLINED.md` at graduation |
| **Written against** | spec, stdlib and rationale **v0.357** (tree `e49c32aca`) |
| **Change class** | Design change (spec-change-process §2). Part 1 is a clarification with a doctrine change; parts 2 and 3 add surface |
| **Origin** | this note; the owner's question of 2026-11-10 |

Sections:

0. **Summary.** The marker states origin; the caller states stance; the handler decides.
1. **Scope and words.** In: the restatement, the failure-to-defect conversion, the query
   surface for arithmetic. Out: any change to which classes are marked, the six misfits,
   the `(try E)` atom itself, a warning on catch-alls (proposal 0213). One owner question on
   words: keep *defect* for an unmarked class, or say *unclassified*.
2. **Evidence.** §3 to §5 of this note, after an independent relabelling of the table.
3. **Part 1: the restatement.** Proposed sense of the amended sentences:
   - spec §31.4: *A class that implements `IFailure` states where its condition comes from:
     outside the program. That is the raiser's statement and a default for handlers. It does
     not say that the program can carry on. A class without the marker makes no statement:
     its exception may report a bug, and a handler that does not know better treats it as
     one.*
   - stdlib §1: *The kind states origin. What a program does about a failure (continue,
     report, stop) is its handler's decision and no part of the kind.*
   - Guide 2 §4: the three sentences of §1; "an `IFailure` arm belongs where every failure
     beneath it has the same answer"; the three call-site questions of §3.5.
4. **Part 2: "a failure here is a defect".** What exists (QUOTED surface, all landed):
   `Exception`'s `(message, cause)` constructor (spec §31.4); the rule that an `Exception`
   raised as itself, like any unmarked class, is a defect (0211 §4.2); `ParseOrNull` and
   `!`. The idiom today:

   ```bezel
   // v0.357 text; not compiled: no released compiler implements the marker
   public static method ReadSchema(path: string): string
     try
       return File.ReadAllText(path);
     catch
       var f: IFailure => throw new Exception($"{path} ships with the service", f)
     end;
   end ReadSchema;
   ```

   The proposal chooses among: (a) teach this idiom and nothing more; (b) ▲ a library
   member that takes a function, if spec §14.4 lets it return an owned result; (c) ▲ an
   expression form beside the deferred `(try E)`, so that the caller's two stances are
   designed as a pair. Requirements for (b) and (c): the cause is kept in `CauseChain`; an
   owned result stays owned; cancellation and defects pass through unchanged. Which class
   the conversion raises is part of the design: the taxonomy "stays deliberately small"
   (QUOTED, decline index, the `NotSupportedException` row), and of the built-in classes
   only `Exception` declares a `(message, cause)` constructor (spec §31.4).
5. **Part 3: query forms for data-caused conditions.** ▲ Checked-arithmetic queries on the
   integer types (nullable-returning, by the stdlib §1 `*OrNull` tier), and a validity query
   for date components. May be split into its own stdlib proposal.
6. **Interactions.** Spec §31.2.3 (arm order is unchanged); §30.14 (a converted failure
   joins as a defect); §22.16 (contracts are untouched); §32.8 (unchanged).
7. **Recorded declines and gates.** The table below.
8. **Migration and conformance.** No rule case changes or is deleted; the new cases;
   the rule-corpus sweep of spec-change-process §5 item 3c for the amended sentences.
9. **Owner questions.** §8 of this note.

**Recorded decisions touched, and the new evidence for each.**

| Decision | Touched how | New evidence |
|---|---|---|
| 0211 §16 Q6; decline row *The exception kind following the tier* | **Not reopened**: one class keeps one kind. Its consequence is addressed by part 2 | 26 of 27 failure-typed throwing sites in the everyday programs are assert-stance sites (§5.2); 11 sample rows (§3.4); the v0.355 glosses (§5.1) |
| 0211 §16 Q7; decline row *Arithmetic over external data … as a failure* | **Its bar names the remedy** ("a standard-library checked-arithmetic query surface"); part 3 proposes that surface. The classification is not reopened | 10 of 13 overflow rows are expected results of the data and 2 are bugs; 5 of the 11 boundary rows the marker arm passes are overflows; `checked_add`, `Math.addExact`, `addingReportingOverflow` as prior art (from memory) |
| 0211 §16 Q8 (the taxonomy as tabled) | **Not reopened; restated as origin** | 94% against 42% (§3.1) |
| 0211 §16 Q10; decline row *Re-typing the six recorded failure-class misfits* (deferred) | **Untouched.** Part 2 does not fix them: the raising member is the one that knows | T13f, T20f |
| 0211 §16 Q12; decline row *"Unmarked means failure"* | **Untouched**; part 1 rewords what unmarked means without changing what an arm takes | — |
| Decline row *The exception kind as a virtual `IsFailure` property … or a band chosen at the `throw` site* | **Not reopened** | only 2 of the 11 misplaced rows are visible to the raise site (§6, E1) |
| D-0005, *Value-form try expression*; the note's §8.5 gate | **Touched if part 2 takes form (c).** The conversion does not make expected failure easier to handle, which is D-0005's ground; it makes not handling it explicit. The gate on `(try E)` itself is not claimed met | bearing on that gate, for whoever takes it up: 83 of 172 failure-typed rows are handled at the call, and 43 of those are on conditions with no query form |
| Decline row *Abandoning the process … and any rule forcing callers to handle failures* | **Untouched** | — |

## 8. Limits and open questions

**Limits** (beyond §2).

- Nothing here ran in Bezel. The idiom of §7.1 and the claims about arms are read off the
  v0.357 text.
- The prior art of §4.2 is from memory. The Java, Rust, Go and Python behaviour of §4.1 is
  measured; the C#, Swift and Kotlin samples are not compiled.
- The corpora of §5.3 are test programs; the 40 programs of §5.2 carry their data inline.
- The "under a unit boundary" annotation of §3.6 (45 rows) is a single judgement per row.
- The sample has no row for a library that a second library wraps; the layering question
  (who translates what) is argued in §6 B, not measured.

**Open questions for the owner.**

1. **What should the marker mean?** Origin, which the library can state (C), or "this
   application expects it", which only the application can (B)? The evidence separates the
   two; it does not choose.
2. **The words.** *Failure* and *defect* read as a statement about severity. Keep *defect*
   for unmarked classes, say *unclassified*, or leave the words and fix only the glosses?
3. **The form of part 2.** A guide idiom, a library member, or an expression form decided
   together with `(try E)`?
4. **Checked arithmetic.** Its own proposal now, or with part 2?
5. **The cause as a value.** Two of the nine at-risk rows of §3.6 share a class and a call
   with a routine condition (disk full and an unreadable file are both `IOException` from
   one copy). Should `IOException` carry the operating system's reason as a value a guard
   can test, as Rust's `ErrorKind` and Go's `errors.Is` do? This is outside the kind
   question and was met while answering it.
6. **A second rater.** Should the table be relabelled independently before any proposal
   cites its rates?

## 9. Files

| File | Content |
|---|---|
| `data/scenarios.tsv` | the 272 task–operation pairs: condition, the three call-site facts, the first-pass label, one line of justification |
| `data/scenarios_pass2.tsv`, `data/pass2_sheet.py` | the second-pass labels, and the script that prints the blank shuffled sheet they were made from |
| `data/conditions.tsv` | the 35 conditions: the v0.357 class, its kind, whether a query form exists |
| `data/failure_typed_stop_rows.tsv` | where each failure-typed row labelled F or D is raised (§3.6) |
| `data/contextuality.py` → `contextuality_by_type.tsv`, `contextuality_by_condition.tsv`, `results.json` | every number of §3 |
| `data/everyday_sites.py`, `everyday_sites_annot.tsv` → `everyday_sites.tsv`, `everyday_sites_summary.json` | §5.2 |
| `data/corpus_arms.py` → `corpus_arms_by_type.tsv`, `corpus_arms_summary.json` | §5.3 |
| `data/landing_reach.py` → `landing_reach.json` | the costs of §6 |
| `snippets/demo.go`, `demo.rs`, `Demo.java`, `demo.py`, `CheckedForcesHandler.java`, `run.sh` → `RESULTS.txt` | the four samples that were built and run |
| `snippets/Demo.cs`, `demo.swift`, `demo.kt` | the three samples that were not compiled |

To regenerate (scratch under `/var/tmp/bezel-scratch/fk`, never the repository):

```text
X=/var/tmp/bezel-scratch/fk
D=docs/research/failure-kind-by-context/data
mkdir -p $X/export/tests/conformance $X/data
git archive HEAD docs scripts/extract_doc_examples.py | tar -x -C $X/export
python3 scripts/extract_doc_examples.py --repo-root $X/export
python3 docs/research/error-model/probes-0211/data/catch_arms.py . $X/export $X/data
python3 $D/corpus_arms.py $X/data/catch_arms.tsv $D
python3 $D/everyday_sites.py . $D
python3 $D/contextuality.py $D
python3 $D/landing_reach.py . $D/landing_reach.json
docs/research/failure-kind-by-context/snippets/run.sh $X/snip
```
