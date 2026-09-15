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

## Gate wiring

Not yet wired into `scripts/gate_full_cycle.sh`. The wiring lands with the first
classification, for the reason above: before a baseline exists every case is
UNCLASSIFIED, and a phase that can only be green is not a check. The harness unit
tests (`test_rules.py`) are build-free and can be wired into the `gate-selftest`
phase immediately, beside `scripts/gate/tests`.

## Coverage

`coverage.py` enumerates every numbered subsection of the spec and the stdlib,
annotates each with a rule-density estimate and its Appendix C invariant count,
and joins that against the corpus. It answers "which sections have no cases",
which is the question that keeps an authoring effort from drifting toward the
easy sections.

Density is a proxy — it tallies the phrases the documents use when they state a
consequence — not a rule count. It sizes a section's work; Appendix C is the
authoritative rule index, and its 217 rows are the corpus's primary target.

## Record

### Founding — v0.298 (2026-09-15)

Harness, contract, coverage matrix and the first authored batch. Documents at
spec v0.298. No classification oracle: this session's container has clang/LLVM
18 and the compiler needs 21, so every case in this batch is written from the
document, reviewed against it, and lands UNCLASSIFIED. The first dev-box
`--classify` run is the acceptance pass, and its divergence list is the first
spec-catch-up docket the corpus produces.
