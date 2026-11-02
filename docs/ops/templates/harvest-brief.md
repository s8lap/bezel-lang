# Harvest lane brief — template

*Added 2026-11-02, from R37. The self-check below was written in a scratch brief
(`/var/tmp/bezel-scratch/r37-coord/harvest-brief.md`) after the R37 harvests H9 and H10 were
both REJECTED — H10 had 45 of its 116 cases bad on the first review. A rule that lives only in
scratch is a rule the next run does not have; this is the committed copy.*

Copy this file, fill the slots, and dispatch it as the harvest lane's brief. The runbook
section *Harvest review probes into the corpus* holds the procedure; this file holds the
per-case discipline.

## Slots the coordinator fills

- **Harvest lane:** `h<N>` — branch `r<run>-h<N>-harvest`, worktree `/mnt/fast/worktrees/r<run>-h<N>`.
- **Source directories:** the probe directories from the picks in `harvest.tsv` (the reviewer's
  harvest list and the producer's handback name them).
- **Scope:** `tests/conformance/rules/cases/` plus the derived `manifest.tsv`. Baseline rows are
  the integrator's, after the pick — do not edit `baseline.tsv`.
- **Dispatch:** Sonnet, effort **HIGH**. **Size: ordinarily 60 cases or fewer.** The coordinator may
  set a higher number for this lane when a review turned up a genuinely under-tested area, and says
  so here: `____ cases (raised from 60 because ____)`. What the size guards against is a batch
  large enough that one bad helper class silently reaches dozens of cases.
- **Self-check record:** `scratch/h<N>/selfcheck.txt` — one line per case, one line per check.
  Quote its path in the handback.

## The eight-point per-case self-check

Every case passes ALL eight before the lane commits. Record each one in the self-check file.

1. **VERBATIM RULE.** The `rule:` field must be an exact excerpt from `docs/Bezel_Language_Spec.md`
   or `docs/Bezel_Standard_Library.md`. `grep -F` the excerpt and record the matching line number.
   Lane-authored gloss goes in `title:`, never in `rule:`. `cites:` must name the section of that
   line.
2. **VERDICT FROM THE SPEC.** Hand-trace the program against the cited sentence and write the trace
   down. NEVER copy the expected output from a compiler run, and above all never from a lane's or a
   rejected fix's tip. If the trace is ambiguous, drop the case.
3. **FIXTURE LEGALITY.** Every helper class, property, parameter spelling and call site must be
   spec-legal on its own, independent of the rule under test. A case with a second error ground is
   invalid. Known traps:
   - a property of owning-choice / move-only type (§23.5, not admitted);
   - `move` at a move-only argument (§15.3: unmarked at either parameter spelling);
   - a meet of an owned move-only local with a borrow arm (K7.3);
   - retired spellings (`owned` binding marker).
4. **NO HOST-DEFINED BEHAVIOUR.** Do not assert stdout/stderr interleaving (stdlib §2.1), thread
   interleaving, or hash order.
5. **NOT VACUOUS.** The program must actually exercise the rule: a shadowing case needs the
   shadowed import; a leak case needs a 100x loop.
6. **RED ROWS.** The `issue:` must be OPEN, and its body must describe this exact divergence
   (`gh issue view`). If no issue fits, report that instead of guessing.
7. **STDLIB.** Use only names in `Bezel_For_Code_Generation.md` §5 or the stdlib.
8. **SECOND ERROR GROUND FOR REJECTS.** Confirm the diagnostic fires at the single intended site,
   and that the accepting twin compiles.

## Before the handback

```
python3 tests/conformance/rules/corpus.py lint      # must report 0 problems
python3 tests/conformance/rules/corpus.py build     # regenerate manifest.tsv
python3 tests/conformance/rules/corpus.py check     # manifest must be current
python3 tests/conformance/rules/corpus.py audit     # no new twin group vs the base
python3 scripts/gate_rule_corpus_baseline.py --strict
```

Report PASS/RED counts by issue, plus the self-check file path.

## Why each point is here

The R37 failures, each of which reached a reviewer's desk as a "case":

| Failure | Point |
|---|---|
| an invented or paraphrased `rule:` | 1 |
| an expected output copied from a rejected fix's tip run | 2 |
| a spec-illegal shared fixture, which gave every case a second error ground | 3 |
| an assertion on host-defined stdout/stderr order | 4 |
| a vacuous shadowing case | 5 |
| a RED row citing the wrong issue | 6 |

One bad helper class poisoned 32 cases. Hence the checks, and hence the ordinary size.
