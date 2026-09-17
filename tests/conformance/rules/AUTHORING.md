# Authoring a conformance case

This is the contract every authoring lane follows. Read it end to end before
writing a case; it is short on purpose.

## What a case is

**One normative rule, made executable, written from the document.**

A case is a complete `.bzl` source file that a compiler either accepts or
rejects, together with a header saying which the *spec* requires and why. It is
not a regression test: it is pinned to a rule, not to a bug, and its verdict
comes from the document, never from what a compiler happens to do.

The corpus therefore fails on the day it lands, in every place the
implementation has not caught up. That is the deliverable. **Never soften a case
to match a compiler** — you will not have one to run, and a case bent toward an
implementation is worse than no case, because it teaches the wrong rule.

## The three expectations

| `expect:` | Means | Extra header |
|---|---|---|
| `accept` | the file compiles as a library (`bezelc -c`) | — |
| `reject` | the compiler rejects it, per a stated rule | `diagnostic:` |
| `quiet` | it compiles, and the compiler says **nothing** | — |
| `warn` | it compiles, and the compiler warns | `diagnostic:` |
| `run` | the file has a `method Main`, compiles, runs, and prints | `stdout:` or `output:` |

`reject` is the workhorse: most of the spec's content is rules about what is
*not* admitted, and Appendix C is 217 bullets of exactly that.

`warn` is for the handful of rules whose consequence is a diagnostic that does
not stop the compile: the must-use warning on an undischarged plain-`var` owned
local, the fresh-store warning on a borrowing collection, the use-after-dispose
warning, the terminal-accessor provenance warning. Those are normative and an
`accept` case cannot tell them from silence. A `warn` case passes when the
compile succeeds *and* says something; which wording is reported, not gated.

`quiet` is `accept`'s sharper sibling, for a rule stated as a **suppression**.
§28.3 has several — "the warning is suppressed when the local is declared
`using var`, when it is explicitly disposed or discarded, and when it is moved
or returned" — and `accept` cannot tell a suppressed warning from an emitted
one, so a case about a suppression written as `accept` proves nothing. `quiet`
fails if the compiler emits any diagnostic at all.

Beware the confound in the other direction: §28.9 makes a class with no
destructor and no `owned` field **trivial-dispose**, which suppresses the
must-use warning by itself. A `warn` case about discharge must give its class a
destructor, or it is asking for a diagnostic the spec does not require.

`run` is for rules with observable behaviour — evaluation order, disposal order,
overflow, formatting, what an interpolation hole renders, what a `case` selects.
Prefer `run` whenever the rule says what something *does*, not merely that it is
admitted: an `accept` case proves only that nothing was rejected.

`stdout:` is the expected **first line**. `output:` is every line, and its
continuations are lines rather than a wrapped sentence:

```
//! expect: run
//! output: container destructor
//!   second field
//!   first field
```

Reach for `output:` whenever the rule is about **order** or **how many times**
something happened — disposal order, evaluation order, how often a selector ran.
Those are the corpus's most valuable rows and a first-line check cannot see
them: a case that disposes twice when it should dispose once prints the same
first line either way.

### What a `run` case may assume about rendering

A `run` case compares the **first line of standard output**, so its expected text
must come from the documents, never from intuition about how a runtime formats
things. What the documents actually pin:

| Value | Renders as | Where |
|---|---|---|
| `bool` | `True` / `False` | §8.5's worked `IsEmpty: False` |
| `double`, `float` (no format spec) | the **shortest** string that parses back to the identical value — so `10.0` is `10` | stdlib §2.18's round-trip guarantee |
| a null hole in an interpolated string | the four characters `null`, with no dispatch | §18.5, §8.5 |
| a class with no `ToString` override | the **qualified** name (`App.Billing.Account`) | §8.5 |
| a positional record | `Point(X: 10, Y: 20)` — unqualified head | §8.5, §9.1 |
| a body record that is auto-hashable | `TypeName(Prop1: v1, Prop2: v2)` over its auto-properties | §8.5, §9.2 |
| a body record that is **not** auto-hashable, and an enum | the qualified type name — *not* the case name | §8.5, §10.5 |
| `decimal` (no format spec) | the exact decimal form: sign, integer digits, and a `.` with the fractional digits only when there is a fractional part. No exponent, no grouping separator, **no trailing zeros** — scale is not part of the value, so `1.50m` and `1.5m` both render `1.5`, `10m` renders `10`, `0m` renders `0` | stdlib §9.3 *Rendering and range* (v0.300), §2.18 |
| an inline array, the unit value | `[3]int`, `()` | §8.5 |
| `{x:F2}`, `{x:X}`, `{x:P1}` | stdlib §2.18's table, invariant culture, no grouping separators | stdlib §2.18 |

An `int` prints its digits. A `decimal`'s rendering is pinned as of v0.300 (the
row above) — it was not when this contract was first written, and the old advice
to avoid decimals cost the corpus `run` cases. Note the trailing-zero rule in
particular: a case that expects `1.50` is wrong, because that value renders `1.5`.

A **choice**'s `ToString` head is the one rendering still unpinned: §35.7 fixes the
shape `TypeName.CaseName(field: value, …)` but neither it nor §8.5 says whether
`TypeName` is qualified. Do not print a choice in a `run` case; observe the rule
directly instead, and say so in your report.

If you cannot pin the exact first line from the documents, the case is an
`accept` case, or prints something derived that you *can* pin. Do not guess.

### A `run` case that must also warn

Some rules have two halves that only one program shows. §28.2's second
`Dispose()` is a runtime no-op **and** draws the compile-time use-after-dispose
warning (v0.299, #4399); §29.7's is the same shape. `warn` throws away the
runtime observation that is half the rule, `run` cannot see the diagnostic, and
two cases would duplicate the program and let the halves drift apart.

A `run` case may therefore carry `warns: yes`, which asserts that the compile
also said something. It takes `yes` or nothing, it is rejected on any other
tier, and — as on a `warn` case — *which* diagnostic is the diagnostic report's
business, not the gate's. The stated output is still checked in full: the
warning is an additional requirement, never a way to pass without running right.

```
//! expect: run
//! warns: yes
//! output: disposed
```

## The pairing rule

**A reject case wants an accepting twin that differs by the smallest legal
edit.** Rejecting `owned _n: string` proves little on its own — a compiler that
rejects the whole file for an unrelated reason also "passes". The twin
(`owned _n: Connection`, everything else identical) is what makes the pair
isolate the rule. Write the pair together, in the same section, adjacent ids.

The same discipline applies inside a case: change **one** thing away from legal
code. A case that breaks two rules at once passes for the wrong reason the day
one of them is fixed.

## The header

```
//! case: s28_5_0002              must equal the filename stem
//! doc: spec                     spec | stdlib
//! section: 28.5                 must agree with the case id
//! title: owned on a string field is rejected
//! rule: The declared type of an `owned` slot, nullability stripped, must be a
//!   class type, an interface type, or a bare type parameter.
//! cites: 28.1, 28.9             optional, comma-separated, no § sign
//! expect: reject
//! diagnostic: owned             see below
//! issue: 4154                   optional: a tracker this case pins
//! flags: --checked-disposal     optional: a non-default build mode
```

`flags:` asks for a compiler switch, from a short allowlist:
`--checked-disposal` (§28.2's checked disposal mode) and
`--experimental-scoped-await-bind`. A rule that only holds in a non-default mode
needs its switch, and a case that does not name one is checked in the default
build.

Continuation lines start with `//!` and **two or more** spaces. Everything below
the header is ordinary Bezel.

Case ids: `s<section>_<NNNN>` for the spec (`s28_5_0002` is §28.5 case 2),
`l<section>_<NNNN>` for the stdlib (`l4_9_0003`). Dots in the section become
underscores. Number sequentially from `0001` within each section; your lane owns
its sections outright, so no other lane can collide with you.

`rule:` is the normative sentence, quoted or tightly paraphrased from the
document — close enough that a reader can find it. It is what makes the case
reviewable without a compiler, so it is not optional and not a restatement of
the title.

`diagnostic:` (on a `reject` or a `warn` case) is **the wording the rule itself
uses** (`owned`, `must announce
its classification`, `not disposed on every path`), not a guess at the
compiler's message. It is reported, never gated: at classification time a
mismatch means either the diagnostic does not name the rule it enforced, or the
case tripped a different rule than it meant to. Both are findings worth having.
Keep it short — a distinctive fragment, lowercase, no punctuation to match.

## When one file is not enough

A few rules are *about* the boundary between compilation units and cannot be
written as one file: §2.8's artifact-set rules, §13.4 and §13.1's cross-module
visibility rows, §26.5's resolution against an *imported* extension block. For
those, a case is a **directory**:

```
cases/s13_4_0001/
  lib.bzl      a dependency: its own module, no //! header, no `method Main`
  main.bzl     the case proper: carries the //! header, imports the dependency
```

Dependencies compile first, in lexicographic order, each one importing the
artifacts built before it; `main.bzl` compiles with all of them imported. So
`lib.bzl` sorts before `main.bzl` naturally, and a second dependency named
`aaa.bzl` would come first — name them for the order you want.

**A dependency that does not compile fails the case**, whatever its expectation:
the rule under test lives in `main.bzl`, so a broken dependency is a broken
case, never a passing reject.

A dependency is a library and normally may not hold a `method Main`. §2.8.4's
rules *about* a qualifying `Main` in a dependency artifact — that it is inert,
and that presenting an executable as a dependency is a build error — are the
exception, and opt in with `notes: dep-main`.

**One module is one file.** `bezelc` takes exactly one input, so a case cannot
put two files in one compilation: two dependency files declaring the same module
name are a duplicate-module-name build error, not a shared compilation. That
puts §2.8.1 rule 2's positive half and every import-SCC rule out of reach — say
so in your report rather than approximating them.

Use this form only when the rule is really about the boundary. A rule that can
be shown in one file belongs in one file.

## Writing the Bezel

**Read `docs/Bezel_For_Code_Generation.md` first**, all of §3 (syntax surface),
§4 (ownership) and §7 (*looks right, is rejected*). It is the writing reference,
at a fifteenth the size of the spec. Then read your assigned spec sections in
full. Where the two disagree, **the spec wins** and the disagreement is a defect
to report in your lane report.

Hard rules, each of which has cost somebody a day:

1. **Do not invent stdlib API.** Every type and member must be in
   `Bezel_For_Code_Generation.md` §5 or `Bezel_Standard_Library.md`. If what you
   need does not exist, declare your own class in the case — a case is
   self-contained and may declare whatever it needs.
2. **Each case is one whole file.** It starts `module Conf;`. No preamble, no
   ambient bindings, no illustrative names the file does not declare. Every type
   the case mentions is either a primitive, a documented stdlib name, or
   declared right there. (A rule that genuinely needs two compilation units
   takes the directory form below — do not fake one inside a single file.)
3. **Prefer an explicit constructor, but know it is a preference.** Writing
   `public constructor() end constructor;` keeps a case self-evident. It is no
   longer a correctness rule: §22.7 (v0.300) states that a class declaring no
   instance constructor and no primary-constructor parameter list **has** an
   implicit parameterless constructor, and that it is what `new C()` reaches;
   §14.2 makes such a class satisfy `where T: new()`. Until v0.301 this rule
   claimed the opposite and the lint enforced it, which rejected the very
   program #4364 was written to admit. Rely on the implicit constructor freely
   where the case is *about* it; declare one where the case is about something
   else and the reader should not have to think about it.
4. **Terminators are load-bearing.** Every statement ends `;`; every block ends
   `end …;`; a lambda's `do … end` block takes no `;` of its own. §3.4 of the
   writing reference has the exact closer for every construct.
5. **`owned` precedes the identifier** (`owned p: T`, never `p: owned T`), and
   `move` is a use-site marker whose *absence* and whose *presence* are both
   compile errors in the positions §4.2 lists. If your case is not about that
   list, do not stray into it.
6. **Bind owned locals with `using var`** unless the case is *about* the
   must-use warning. A plain `var c := new C();` left undischarged compiles, but
   it emits the must-use warning, and a case carrying a diagnostic it is not
   about is a case whose next reader has to work out which one mattered.
7. **Keep it small.** Ten to thirty lines. A case that needs a paragraph of
   scaffolding is testing the scaffolding.
8. **No `TODO`, no `...`, no placeholder names** — the lint rejects them. A case
   is complete code.

## Tracing before writing

For every case, in your head or on paper:

- What does the section actually say? Quote it into `rule:`.
- **For a reject case, apply the deletion test**: delete the offending
  construct and confirm the remainder compiles. If it does not, the case is not
  isolated — it has a second, earlier ground for rejection and will pass for the
  wrong reason, hiding the real gap forever. Failing to parse is the obvious
  version of this; a *semantic* second ground is the one that actually gets
  through, and in this language it is almost always an ownership rule the case
  did not mean to touch.
- For an accept case: is there any *other* rule in the language that rejects
  this file? (The usual culprits: an undischarged owned local, a static method
  returning a class type, a missing visibility keyword, a borrow that outlives
  its source.)
- **Does my scaffolding contain a line that is illegal only if the rule
  fails?** The recurring shape is a trailing `x.Dispose();` after the construct
  under test: inert while the rule holds, and the *actual* reason for rejection
  the day the rule is missing — which is precisely when the case is meant to
  fail. Two critique passes found it four times. Delete such a line; the case is
  stronger without it.
- **Is my scaffolding fighting me?** When the rule under test is not about
  ownership, scaffold with a runtime-managed type — `string`, a primitive, a
  record — rather than a class. A class-typed local carries an ownership
  classification and can manufacture a collateral error out of nowhere: a
  captured binding reassigned inside a closure, a borrow returned under an
  `owned` return, a merge of an owned arm with a borrowed one. `string` carries
  no classification at all and cannot do this to you. Three of the first
  batch's four real defects were exactly this, all in cases about nullability.
- **Am I observing my own rule?** A case that proves its point by rendering
  something is leaning on §18.5's and §8.5's rendering rules. That is fine when
  the rendering is incidental, and wrong when the rendering IS the assertion —
  then the case passes or fails on another section's behaviour while claiming
  yours, and a regression there is reported as a divergence here. Observe your
  rule directly where you can (compare the value, print a marker from the branch
  that ran).
- For a run case: trace every path and say exactly what the first line of
  standard output is.
- **Would the expectation change if the rule were violated?** This is the
  question that catches a vacuous case, and it applies to every tier. A `run`
  case whose expected line is printed on every path — including the path a
  violating compiler would take — proves nothing; a fault counts as a change, so
  arrange the program to fault rather than print if the rule does not hold. An
  `accept` case must **exercise** the admitted operation, not merely declare the
  shape that admits it: binding the borrow a rule says may be *used*, and then
  never using it, proves only that the binding form parses.

The third bullet is where most authoring errors live. An accept case that the
spec rejects for an unrelated reason is a false divergence, and false
divergences are what make a corpus untrustworthy.

## What not to write

- Cases for rules that are *not* compiler-enforced ("should", "by convention",
  naming style — §1.3 says the compiler does not enforce naming).
- Cases whose verdict the section does not settle. If you find one, do not guess:
  record it in your lane report as an open question. A spec gap found this way is
  a valuable finding — several revisions have landed off exactly such reports.
- Cases that restate the grammar. Appendix A is checked by the parser tests; this
  corpus is for rules with stated consequences.
- Duplicates of a case you already wrote in another section. The rule's home
  section owns it.
- **An identity comparison, where the rule is *how* a comparison is computed.**
  `X = X` is true under the correct rule and true under every wrong one — the
  spec's own named-and-rejected "zero-property, every-pair-equal" synthesis
  included. Two cases asserted `IntPtr.Zero = IntPtr.Zero` and
  `Ptr<int>.Zero = Ptr<int>.Zero` over rules that say the comparison reads the
  *address representation*, which an identity comparison cannot observe. A case
  about an equality rule needs a pair the rule separates — something that
  compares **unequal** — beside any equal pair. The same trap catches `=` on
  records, choices and enums.
- **A case for a stdlib type whose semantics live in the spec, cited to the
  stdlib.** The threading sections in particular *declare* and let spec §30.8 /
  §30.12 / §30.14 carry the meaning: stdlib §6.2 nowhere says `Mutex` is
  non-reentrant, and §6.3 states no permit counting. A lane assigned "stdlib §6"
  that reads only stdlib §6 writes a `rule:` the cited section cannot back —
  three of twelve cases in one batch. Cross-read the spec chapter before you
  trust a `rule:` line, and cite whichever document states it.

## Checking your work

```
python3 tests/conformance/rules/corpus.py lint     # structure, ids, bijection
python3 tests/conformance/rules/corpus.py build    # regenerate manifest.tsv
python3 tests/conformance/rules/corpus.py summary  # per-section counts
python3 tests/conformance/rules/names.py           # invented stdlib API
python3 tests/conformance/rules/corpus.py audit    # quality signals, never fails
python3 tests/conformance/rules/coverage.py --invariants 28.5   # the C rows for a section
```

`coverage.py --invariants <section>` prints every Appendix C invariant citing
that section (or any subsection under it). Those rows are the authoritative rule
index — work through them, and say in your report which you covered.

**Appendix C indexes the spec only.** A stdlib lane has no invariants: running
`--invariants 6` from one used to answer with *spec* §6 (integer arithmetic)
where the lane meant *stdlib* §6 (threading) — a wrong answer that looks like a
right one. It now refuses `--invariants lib:6` and points you at `--rules
lib:6`, which is what a stdlib lane wants.

`coverage.py --rules <section>` prints the section's rule-stating sentences,
numbered, as a checklist: every sentence that uses one of the phrases the
documents reach for when they state a consequence. It is a checklist, not a rule
count — one sentence often carries several rules and a few carry none — but
working down it is how a lane avoids covering only the rules that were easy to
see. `--rules lib:4.9` reads the stdlib document instead.

`names.py` indexes every type and member the documents declare and reports any
your cases use that they do not — the mechanical form of rule 1 above. It is a
report, not a proof: its member tier misses inherited and extension members, so
check a finding rather than obeying it. A name it flags is nearly always real.

The lint is structural only — it cannot tell you whether the Bezel is right.
That is what your trace and the critique pass are for.

## Your lane report

Finish with a short report: sections covered, case counts by expectation, every
rule you deliberately did **not** cover and why, every open question the spec did
not settle, and every disagreement you found between the spec, the stdlib
document and the writing reference. The report is as valuable as the cases.
