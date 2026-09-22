# The rule corpus

One hand-authored case per normative rule, written from the documents and keyed
to the section that states the rule.

This is the second of the project's two conformance corpora, and the two are
different in kind:

| | `tests/conformance/` (doc examples) | `tests/conformance/rules/` (this one) |
|---|---|---|
| Where cases come from | extracted from the documents' fenced blocks | written against the documents by hand |
| What a case demonstrates | that an example in the docs is real code | that one normative **rule** holds |
| Indexed by | document and block position | spec section and rule |
| Regeneration | wholesale, by `scripts/extract_doc_examples.py` | never — cases are edited, not regenerated |
| Reject coverage | whatever the docs happened to show | systematic: Appendix C's invariants |

They are separate trees because an extraction overwrites, and a hand-authored
case must survive one.

Neither is the regression suite. The 25,000 gtest and lit cases are pinned to
issues and shaped by how the compiler failed; this corpus is pinned to the spec
and shaped by what the language *is*. The two axes catch different things: a
regression test cannot tell you about a rule nobody has implemented yet.

## The invariant that makes it worth having

**A case states the verdict the SPEC requires, never the verdict the compiler
produces.** A case is never edited to match an implementation. When the two
disagree, that is the corpus doing its job: the divergence list is the
spec-catch-up backlog, with the test already written.

This has a consequence to plan for rather than flinch at: on the day a batch
lands, a fraction of it fails. Gating on "all green" would be gating on the
compiler being finished. The baseline ratchet below is how a corpus that is
allowed to fail still catches regressions.

## Layout

```
cases/<case-id>.bzl   one rule, one complete source file, metadata in a //! header
cases/<case-id>/      a case that needs two compilation units: main.bzl + deps
manifest.tsv          GENERATED from the case headers by corpus.py build
baseline.tsv          the verdict the compiler produces today, per case
coverage.tsv          GENERATED: every documented section against its case count
AUTHORING.md          the contract an authoring lane follows
corpus.py             header parsing, lint, manifest generation
runner.py             run the corpus against a bezelc, under the ratchet
coverage.py           build the coverage matrix from the documents
test_rules.py         unit tests for all of the above (build-free)
```

The manifest is **derived**, so an authoring lane creates files and never edits a
shared one: parallel lanes cannot conflict, and the manifest cannot desync from
the cases. `corpus.py check` fails when it is stale.

## Commands

```bash
python3 tests/conformance/rules/corpus.py lint      # structure, ids, titles
python3 tests/conformance/rules/corpus.py build     # regenerate manifest.tsv
python3 tests/conformance/rules/corpus.py check     # is the manifest current?
python3 tests/conformance/rules/corpus.py summary   # per-section counts
python3 tests/conformance/rules/coverage.py --gaps  # what is not covered yet
python3 tests/conformance/rules/corpus.py audit     # quality signals (never fails)
python3 -m unittest discover -s tests/conformance/rules   # harness unit tests
python3 tests/conformance/rules/runner.py build/bezelc    # run it
```

## The baseline ratchet

`baseline.tsv` records, per case, the verdict the compiler produces **today**.
`runner.py` compares each run against it:

| banked | observed | status | gate |
|---|---|---|---|
| `pass` | pass | OK | green |
| `pass` | fail | **REGRESSION** | fails |
| `fail` | fail | KNOWN divergence | green, listed |
| `fail` | pass | **UNBANKED** | fails — bank it |
| (absent) | either | UNCLASSIFIED | green (`--strict-new` fails) |

The fourth row is the same exact-not-a-ceiling ratchet
`check_deprecated_jit_oracle.py` uses, for the same reason: unbanked headroom is
indistinguishable from a divergence somebody already spent. When a fix closes a
divergence, bank it in the same commit:

```bash
python3 tests/conformance/rules/runner.py build/bezelc --update --filter 28.5
```

A `fail` row is **never** a reason to edit the case. It is a work item; give it
an `issue:` header once it has a tracker.

## Classification

A case with no baseline row has never been run. Classification is the pass that
gives it one, and it needs a current `bezelc`, so it happens on a development
box, not in a cloud session:

```bash
python3 tests/conformance/rules/runner.py build/bezelc --classify \
    --report /var/tmp/bezel-scratch/rule-corpus-classify.tsv
```

`--classify` ignores the baseline, runs everything, and rewrites `baseline.tsv`
from what it observed. Record the oracle's identity (binary date, commit) in the
classification record below, exactly as the doc-example corpus's README does for
its regenerations — a verdict is only as good as the binary that produced it.

The run also reports **diagnostic mismatches**: a reject case carries the
*rule's* wording in its `diagnostic:` field, and where the compiler's message
does not contain it, either the diagnostic fails to name the rule it enforced or
the case tripped a different rule than it meant to. Both are findings. The
mismatch list is reported, never gated.

### What to do with the divergences

The first classification produces a long failure list, and that is the expected
outcome, not a problem with the run. Triage each divergence into exactly one of
three, and record which:

1. **The compiler has not caught up.** The case is right, the implementation is
   behind. Bank the observed verdict, open an issue citing the section and
   revision, and the case becomes that issue's regression test — already
   written, and written from the document rather than from the failure.
2. **The case is wrong.** Fix the case, and ask what let it through: if the same
   defect could recur, it belongs in `AUTHORING.md` as a rule or, better, in
   `corpus.py` as a lint. Three of the contract's rules arrived this way.
3. **The document does not settle it.** Open an issue, quote the passages that
   conflict, and leave the case banked at whatever it does today with an
   `issue:` header pointing at it. Never edit a case to match the compiler —
   that is the one move that turns the corpus back into a regression suite.

Only (1) is a backlog item. (2) is corpus maintenance and (3) is spec work, and
keeping them apart is what makes the divergence count mean something.

## Gate wiring

The **harness** is gated; the **corpus** is not, and the split is deliberate.

`scripts/gate_full_cycle.sh` runs a build-free `rule-corpus` phase beside the
gate self-tests. It runs `test_rules.py` under `unittest discover` against a
floor on the number of tests collected — `unittest` exits 0 for "Ran 0 tests",
so a count is the only thing that distinguishes a pass from a muted directory —
and then `corpus.py check`, which fails a stale `manifest.tsv` or a corpus that
does not lint. The floor is exact, not a ceiling, for the same reason
`scripts/deprecated_jit_oracle.baseline` is: headroom that is not banked is
indistinguishable from a drop somebody already spent.

The corpus itself waits on the first classification. Before a baseline exists
every case is UNCLASSIFIED, and a phase that can only be green is not a check.

## Coverage

`coverage.py` enumerates every numbered subsection of the spec and the stdlib,
annotates each with a rule-density estimate and its Appendix C invariant count,
and joins that against the corpus. It answers "which sections have no cases",
which is the question that keeps an authoring effort from drifting toward the
easy sections.

Density is a proxy — it tallies the phrases the documents use when they state a
consequence — not a rule count. It sizes a section's work; Appendix C is the
authoritative rule index, and its 217 rows are the corpus's primary target.

`coverage.tsv` is checked in and regenerated by `coverage.py --gaps`. One row
per numbered section, with its density, its Appendix C row count, its case
counts by expectation, and a `state` of `UNCOVERED`, `thin`, `covered` or `n/a`
(a section that states no enforced consequence). Reviewing the diff of that file
is how a wave's real effect is read: a wave that moves case counts without
moving any section out of `UNCOVERED` went over ground already held.

Two states say a gap is not real, and they are different claims. A **container**
— a numbered heading whose own body is empty, all of whose text lives in its
subsections — is `n/a`, decided by measurement: Appendix C rows citing a
subsection are attributed to the parent as well, so a container otherwise reads
as a dense uncovered section while stating nothing at all. **`covered-elsewhere`**
is a judgement, so it is never inferred: it is claimed in `coverage_acks.tsv`,
one row per section, naming the case that covers the rule and why. The claim is
checked — an ack naming a case that does not exist is a lint error, because a
wrong ack hides a real gap permanently, which is worse than the false gap it
removes.

A section is covered when a case exists for it, which is weaker than every rule
in it being covered. The stronger question is per-rule, and
`coverage.py --rules <section>` and `--invariants <section>` are what answer it
— a numbered checklist of the section's rule-sentences and of the Appendix C
rows citing it. Those are the authoring tools; `--gaps` is the review tool.

## Record

### Founding — v0.298 (2026-09-15)

Harness, contract, coverage matrix and the first authoring run. Documents at
spec v0.298.

**No classification oracle.** The session's container had clang/LLVM 18 and the
compiler needs 21, so every case here is written from the document, reviewed
against it, and lands UNCLASSIFIED. The first dev-box `--classify` run is the
acceptance pass, and its divergence list is the first spec-catch-up docket the
corpus produces.

**How it was written.** Roughly twenty authoring lanes, one per spec area, each
reading its sections and the code-generation reference in full and working down
`coverage.py --rules <section>` and `--invariants <section>`. Then adversarial
critique passes over each batch. The critiques found defects at a steady rate —
7 in the first 49 cases, 11 in the next 46 — and, more usefully, named three
contract defects present in every lane's output. All three are now in
`AUTHORING.md`:

1. **the deletion test** — delete a reject case's offending construct and the
   remainder must compile, or the case has a second, earlier ground and will
   pass for the wrong reason forever. Every instance found was an ownership rule
   the case did not mean to touch;
2. **scaffold with a runtime-managed type** when the rule is not about
   ownership — `string` carries no classification and cannot manufacture a
   collateral error, where a class-typed local routinely does;
3. **non-vacuity** — would the expectation change if the rule were violated?
   Three positive cases could not have failed for their own rule.

The harness grew twice in response to what lanes hit: `output:` (every line of
standard output, not just the first) because disposal ORDER is the corpus's most
valuable observation and a first-line check cannot see it; and directory cases
because §2.8's artifact rules, §13.4's cross-module visibility and §26.5's
imported extension blocks are *about* the boundary between compilation units.

**Findings.** Forty-odd questions the documents do not settle, filed as tracker
issues from #4362, with #4383 as the index. The sharpest are a §7.9
self-contradiction (a non-nullable local must be assigned before it is read; a
*defaultable* local may be declared without an initializer — `int` is both),
§9.2's member list omitting fields that §9.7 and Appendix A both assume, and
#4384: `bezelc` takes exactly one input file, so the multi-file compilation that
§2.8.1's visibility rules and §2.5's SCC rules are written against has no
expression in the compiler's interface at all.

**Known limits of the format.** Some rules cannot be cases: an ABI fact with no
source-visible consequence (§32.5's two-slot `RawBytes` marshalling), a
type-level invariant with no accept/reject surface (§7.10's `?` idempotence), a
build-request property (§2.8.4's artifact kind, `--main-class`), and anything
needing two *versions* of one module (§2.8.3's staleness clause, §2.8.6's
interface delta). Each is recorded where a lane hit it rather than approximated.

### First classification — v0.306 (2026-09-21)

The acceptance pass the founding record was waiting for. Run on a dev box with
a current toolchain, against `build/bezelc` at commit `c52916d9` (integration
branch `r18-c7`), documents at spec v0.306.

```bash
python3 tests/conformance/rules/runner.py build/bezelc --classify --jobs 6
```

**2149 cases: 1736 pass, 413 divergent (80.8%).** Divergence by tier:

| tier | cases | divergent | pass rate |
|---|---|---|---|
| `accept` | 352 | 32 | 91% |
| `reject` | 788 | 148 | 81% |
| `run` | 918 | 185 | 80% |
| `quiet` | 42 | 4 | 90% |
| `warn` | 49 | **44** | **10%** |

The `warn` row is not 44 separate defects. The compiler implements a handful of
warnings (the mutable-static forms in §22.12/§22.13, the fresh-producer forms in
stdlib §4.4/§4.9) and essentially nothing else: must-use and unused-result
(§17.7, §17.13), case exhaustiveness (§19.6), constant-range direction
(§16.4–§16.6), chain shadowing (§2.7) and enum-static sharing (§10.4) all
"compiled silently; the spec requires a warning here". The warning surface is a
single unimplemented area, and it is the corpus's largest finding.

Of the 185 `run` divergences, 124 do not reach execution at all — the case
compiles with an error, so they are over-rejections wearing a `run` label. The
remaining ~61 are wrong behaviour, and they are the severe end: two LLVM IR
verifier failures (`s2_8_2_0005` "Call parameter type does not match function
signature", `s17_5_0004` "Invalid operand types for ICmp instruction"), several
omitted disposals (`s28_3_0043`, `s35_5_0006`, `s35_9_0001`, `s28_12_0005`), one
cleanup that runs twice (`s28_12_0006`), and disposal-ordering violations
(`s15_1_1_0005`–`0007`, `s35_4_0005`).

Per the triage rule above these are class (1) — the compiler has not caught up —
until a case is shown wrong. None were edited to match the compiler.
