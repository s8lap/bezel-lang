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
| `warn` | it compiles, and the compiler warns | `diagnostic:` |
| `run` | the file has a `method Main`, compiles, runs, and prints | `stdout:` |

`reject` is the workhorse: most of the spec's content is rules about what is
*not* admitted, and Appendix C is 217 bullets of exactly that.

`warn` is for the handful of rules whose consequence is a diagnostic that does
not stop the compile: the must-use warning on an undischarged plain-`var` owned
local, the fresh-store warning on a borrowing collection, the use-after-dispose
warning, the terminal-accessor provenance warning. Those are normative and an
`accept` case cannot tell them from silence. A `warn` case passes when the
compile succeeds *and* says something; which wording is reported, not gated.

`run` is for rules with observable behaviour — evaluation order, disposal order,
overflow, formatting, what an interpolation hole renders, what a `case` selects.
Prefer `run` whenever the rule says what something *does*, not merely that it is
admitted: an `accept` case proves only that nothing was rejected.

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
```

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
   declared right there.
3. **Declare constructors explicitly.** Write `public constructor() end
   constructor;` rather than relying on an implicit parameterless one: the spec
   does not state that a class with no declared constructor has one.
4. **Terminators are load-bearing.** Every statement ends `;`; every block ends
   `end …;`; a lambda's `do … end` block takes no `;` of its own. §3.4 of the
   writing reference has the exact closer for every construct.
5. **`owned` precedes the identifier** (`owned p: T`, never `p: owned T`), and
   `move` is a use-site marker whose *absence* and whose *presence* are both
   compile errors in the positions §4.2 lists. If your case is not about that
   list, do not stray into it.
6. **Keep it small.** Ten to thirty lines. A case that needs a paragraph of
   scaffolding is testing the scaffolding.
7. **No `TODO`, no `...`, no placeholder names** — the lint rejects them. A case
   is complete code.

## Tracing before writing

For every case, in your head or on paper:

- What does the section actually say? Quote it into `rule:`.
- Does my code reach that rule, or does it die earlier on something else? A
  reject case that fails to parse proves nothing about a semantic rule.
- For an accept case: is there any *other* rule in the language that rejects
  this file? (The usual culprits: an undischarged owned local, a static method
  returning a class type, a missing visibility keyword, a borrow that outlives
  its source.)
- For a run case: trace every path and say exactly what the first line of
  standard output is.

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

## Checking your work

```
python3 tests/conformance/rules/corpus.py lint     # structure, ids, bijection
python3 tests/conformance/rules/corpus.py build    # regenerate manifest.tsv
python3 tests/conformance/rules/corpus.py summary  # per-section counts
python3 tests/conformance/rules/names.py           # invented stdlib API
python3 tests/conformance/rules/coverage.py --invariants 28.5   # the C rows for a section
```

`coverage.py --invariants <section>` prints every Appendix C invariant citing
that section (or any subsection under it). Those rows are the authoritative rule
index — work through them, and say in your report which you covered.

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
