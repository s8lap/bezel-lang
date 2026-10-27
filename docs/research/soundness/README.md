# How to know an ownership-rule change has not regressed soundness

*Research lane, 2026-10-27, against spec v0.344. Non-normative: everything here is a recommendation,
and nothing in the specification, tests or compiler changed with it. Prototypes: `pinmap/` and
`bzcore/` beside this file; `data/` holds the issue extract §2.5 counts from.*

## 1. One page for the owner

**The question.** When §28 / §30.5 change, how do we know the rules are still sound and that
nothing already decided has quietly flipped?

**What exists today, and what each actually catches.**

| Mechanism | Catches | Misses |
|---|---|---|
| Adversarial critic + composition gate (process §4) | a ruling that contradicts a neighbouring passage; the killer counterexample a strong reader finds | anything two individually-sound rules do *together* three sections apart; the critic reads text against text |
| The three completeness matrices (397 cells, 67 routes) | a cell the current text does not decide, with a ≤25-line escape — **60 root causes at the first runs, 65 spec-defect issues in October, essentially all of the spec-level soundness findings this year** | regressions between runs: a run costs five lanes and a consolidation pass, so it is run per train, not per ruling; cells are *carried* when their quotes still hold, so a re-worded rule whose quote survives is never re-judged |
| The rule corpus (3,343 cases, 435 in §28/§30.5) + baseline ratchet | a compiler verdict moving against the spec's | a *spec* change: no case cites a kernel clause, 242 of the 435 quotes are paraphrases, so nothing mechanical links a clause to the cases that pin it |
| gtest/lit (25k tests), ASan regression matrix | compiler regressions, pinned to issues | spec soundness by construction (they test the implementation) |
| Proposal 0047 (Lean model, T1–T3) | composition bugs across *all* programs of a fragment | pending since July; no toolchain adopted; a large bet |

The soundness record (475 `soundness` issues) says where the holes actually were: **325 are
compiler defects** (213 filed in October alone), **65 are spec defects or questions**, all 65
filed in October by the matrix runs, **82 are spec-ahead-of-impl trackers**. Before the
matrices, the spec-level holes (v0.180 freeze, v0.184 joins, v0.196 frame capture, #1465 /
#2010 drop-on-overwrite) were found by probing the compiler, years late. The kind of rule
change that broke things was always a *composition*: a new admission (a frame, a span, a
borrow-bearing record, an `Object` substitution) meeting an existing kill (`move`, `send`,
overwrite, `Dispose()`) at a position no single rule looked at.

**Recommendation, in priority order.**

1. **Make the existing matrix and corpus checks mechanical, now (one train, small).** Promote
   the trains' scratch tools (`matrixquotes.py`, `rulequotes.py`) and the `pinmap` prototype
   into `scripts/check_docs.sh`: a §28/§30.5 edit must name the K/A clauses it touches, and
   the check prints the matrix cells and corpus cases that cite those clauses or whose quoted
   text changed — the re-derivation worklist — and fails if a listed cell is carried unchanged
   or a listed case keeps its old `rule:`. Add a `kernel:` header (K/A ids) to every §28/§30.5
   case and a ratchet that no live clause has zero pins. This turns "re-run the matrices" into
   "re-judge these 9 cells", which is what v0.336's targeted run already did by hand.
2. **Build the executable reference model, in Python first (two to three trains, medium).**
   The `bzcore` prototype — K1, K3, K4.2, K5, K7, K3.14 over locals, ~450 lines — already
   accepts 50,529 of 308,526 generated programs with zero T1/T2/T3 violations and, under rule
   mutation, finds the v0.180 witness in four statements. Grown to 0047's full Layer 1
   (fields, temporaries, exceptions, `using var`) and tied to `bezelc` through the 0047 §5.3
   printer, it is three tools in one: a mutation oracle for the *spec* (which clauses are
   load-bearing), a differential oracle for the *compiler* (where 325 of 475 holes are), and a
   second judge for corpus cases inside its fragment. It is also 0047's S0 made executable.
3. **Mechanized proof (0047), staged behind 2.** Do S0 as the paper twin of the Python model
   now; take the Lean S1 bet (T1 exactly-once, T2 determinism, then T3ₓ freeze necessity and T3
   isolation) only once the model and bridge are stable, because the proof's definitions will
   be the model's. Frames (T4) last; the trust clauses (K4.3) enter as named hypotheses, never
   as proven facts.
4. **Rule mutation and metamorphic tests as a standing measurement, not a one-off.** Mutation
   of the *model* (above) measures which spec clauses matter; mutation of the *compiler's*
   checks against the corpora measures how much the corpora pin — run it on a dev box per
   soundness train and record the surviving-mutant list in `docs/research/`.
5. **Make the consume table data.** One machine-readable twin of the K3 table beside the spec,
   from which the model's rule table, the disposal matrix's R/C definitions and the
   code-generation reference's §7 lines are generated. Most spec-level soundness defects of
   October were table–prose disagreements; a single source removes the class.

**What this buys and what it costs.** Item 1 is a few days of lane work plus a 435-case header
sweep and closes the regression gap at the *recording* level: no ruling can move text a cell or
case quotes without a worklist. Item 2 is the only item that finds *new* composition holes
before a probe does, at a cost of two to three trains and no new dependency. Item 3 is the
only item that can say "every program", at the cost 0047 §8 estimates (2–4 focused runs for
S1) and a Lean toolchain; its value is capped by the fragment it covers, which is why it
follows 2. Nothing here replaces the matrices' human judgement; everything here makes it
re-runnable on demand and tells it where to look.

---

## 2. What exists, in detail

### 2.1 The kernel and the register (spec §28.3.1–§28.3.2)

Nine principles K1–K9 with 55 numbered sub-clauses (the K3 consume table's rows counted), and
an axiom register A1–A15 (eleven live, four tombstones). Every §28 / §30.5 rule is tagged
`derives: K…` or `axiom A…` (proposal 0046's trains, audited in `0046-audit-v0.199.md`), and
process §9 obliges every §28/§30.5 ruling to state its kernel classification. The pin map
(`pinmap/out/pinmap.md`) reads 113 blocks in §28/§30.5; 56 carry a tag. The tags are the
hinge every mechanical approach below turns on: they are the only place the spec says *which
clause a rule depends on*.

### 2.2 The matrices (`docs/research/{borrow-flow,disposal,generics}-matrix`)

Each run fills every row × column cell with a verdict, a verbatim quote of the deciding
sentence (mechanically checked against the text — `check_quotes.py`), a note, and for an open
cell a ≤25-line counterexample filed as a root-cause issue. Record:

| matrix | runs | first-run open cells | root causes filed | state at v0.336 |
|---|---|---|---|---|
| borrow-flow | 6 (v0.330–v0.336) | 32 / 117 | 10 + 7 + 2 + 3 + 2 | 0 open, 19 routes |
| disposal | 3 (v0.333–v0.336) | 51 / 140 | 21 + 8 + 2 | 0 open, 26 routes |
| generics | 3 (v0.333–v0.336) | 61 / 140 | 29 + 8 + 1 | 1 ambiguous, 22 routes |

Two properties matter for regression use. (a) **Cost**: a run is five lanes plus a
consolidation pass; v0.336 introduced the *targeted* run (re-derive every cell whose quote no
longer holds verbatim, every open cell, every cell the train names; carry the rest) — 66 of 140
disposal cells, 68 of 117 borrow-flow cells. (b) **Selection is manual**: which cells "the
train names" is a lane's judgement. The composition work dirs (`scratch/v0338…v0340/comp_work/
tools/matrixquotes.py`) already compute the quote-staleness half mechanically (v0.340: six cells
lost a fragment, verdicts unchanged) but the tool lives in scratch and runs only inside a
train's composition step.

### 2.3 The corpora

- **Rule corpus** (`tests/conformance/rules`): 3,343 hand-written cases, one per rule, verdict
  from the spec never the compiler; `baseline.tsv` banks what the compiler does and the gate
  fails on REGRESSION/UNBANKED rows. 435 cases cite §28.x or §30.5 (156 cite §28.3 as a
  whole). `cites:` fields name sections and Appendix C, **never a K/A id**. The `rule:` field
  is quoted verbatim in 193 of the 435 and paraphrased in 242 (whole corpus 1,099 / 3,336).
  Process §5 item 3c makes a verdict-moving ruling sweep the corpus "by rule text and
  construct" — a grep over paraphrases.
- **Doc-example corpus** (`tests/conformance/cases`): every fenced example, 138 live rows at
  the last regeneration; proves the docs' examples compile and run, not that rules hold.
- **gtest/lit**: 25k tests pinned to issues; 1,021 source files name §28.3. They catch
  compiler regressions, never spec ones.

### 2.4 How trains catch regressions today

Process §3 (mechanical: anchors, headers, example extraction, grammars, site), §4 (critic per
ruling; composition gate over the union of trains), §5 3c (corpus sweep), §9 checklist (kernel
classification; body-locality classification per 0105). The v0.339/v0.340 trains show the
machinery working at its best — and its limit: the §27.1.1 contravariance escape at `ref`/`out`
slots was found by B2's *critic*, two soundness folds by the *composition* reviewer, and the
corpus and matrix staleness by scratch scripts a composition lane wrote for that train.

### 2.5 Where soundness bugs were found late

From the 475 `soundness` issues and the changelog's soundness notes, the spec-level episodes
and what found them:

| episode | rule change that broke it | found by | revisions late |
|---|---|---|---|
| v0.180 consume-site freeze | `Domain.Spawn(W, move x)` with a live borrow `b` of `x` — a transfer with no flag | adversarial probe against the compiler | §30.14's isolation claim normative since v0.133 |
| v0.184 join refinements | conditional `Dispose()` then read | probe | — |
| v0.196 frame-carried capture | an iterator frame over a capturing callback passed the frame-return check "by the letter" | review re-trace | — |
| #1465, #2010/#2016 drop-on-overwrite × borrow | reassignment disposes under a live borrow | probes | — |
| Oct 2026: #5679–#5693, #5706–#5710 (spans, frames, `Object` substitution) | span-carried borrows had no flag where K5 could not see them; `Keep<Object>.Put(f)` erased a capture set | **borrow-flow matrix runs 1–2** | the span family landed many revisions earlier |
| Oct 2026: #5759–#5789 (second `Dispose()`, restoration windows, cycles via `move h.f`, chaining faults) | table–prose disagreements and K9.2 windows | **disposal matrix run 1** | — |
| v0.339 §27.1.1 `ref`/`out` contravariance | a new admission in §27.1.1 did not exclude aliased slots | critic (B2) | same train |

The pattern: every one is a *new admission* (a value kind or position) meeting an *existing
kill*, and the rule that admitted it was locally correct. That is the composition class 0047
§1.2 names, and it is also exactly the class a matrix cell (kind × position) or a generated
program (kill × live borrow) is shaped to hit.

---

## 3. The five approaches, weighed

### 3.1 Matrix-based regression as a mechanical gate

**What is possible now.** Three mechanical signals already exist in pieces: (i) *quote
staleness* — a cell's `rule` fragments no longer verbatim in the docs (`matrixquotes.py`); (ii)
*clause citation* — cells cite K/A ids in `cite` (K4.1 in 42 cells, K9.4 in 42, K4.3 in 37,
K5.6 in 23 …), so a ruling that names its touched clauses selects the cells to re-judge; (iii)
*section citation* for the rest. The pin map joins all three in one script.

**What a gate would look like.** A new `check_docs.sh` stage, run on every revision touching
§28/§30/§9.7/§15/§35 (the matrices' sources):

1. Diff the normalized docs against the previous revision; list every cell and route whose
   quoted fragments were lost or whose cited clause the changelog entry names.
2. Require each listed cell to carry, in a new `runs/v0.NNN.json`, either a re-derivation
   (`rev` at the new revision, quotes re-located, verdict stated, `prev_verdict` recorded) or an
   explicit `carried-with-reason`. A listed cell silently carried fails the check.
3. Print the verdict diff (`verdict ≠ prev_verdict`) as the revision's *matrix ledger* in the
   changelog's "Matrix status" block — today written by hand.

**What it catches.** Any re-wording or re-ruling of text a cell quotes; any verdict that moves
without a stated reason; a ruling that forgets to name the clause it changes (the cell still
loses its quote). **What it misses.** A cell whose quote survives but whose *meaning* changed
through a neighbour (the K5 (ii) rewrites did this) — only a human re-judging the cell sees it,
which is why step 2 keeps the judgement human and only mechanises the *selection*. It also
misses what no cell covers: the matrices' axes are value kinds × positions; equality, field
restoration and the exit pass were "no cell" in several trains' matrix-status blocks and got
route cells later.

**Cost.** Small: the tools exist in scratch (≈200 lines), the run format already has `rev`,
`prev_status`, `carried`. The expensive half — re-judging — stays per selected cell, typically
5–20 cells per ruling instead of 400 per train. A full re-run every N trains stays worthwhile
as an independent re-read.

### 3.2 An executable reference model as a differential oracle

**What the prototype shows** (`bzcore/`): a checker written clause-by-clause from §28.3.1
over 0047's A-normal locals fragment, with an interpreter whose heap carries K4.1's flag and a
`send` sink with no flag (K4.3's domain clause). Baseline: 308,526 straight-line programs
(3 vars, ≤5 statements), 50,529 accepted, 5,760 with warnings, **0 violations** of T1
(exactly-once), T2 (no silent use-after-kill) or the one-sink T3; with a branch: 67,002
programs, 12,327 accepted, 0 violations. The rejection histogram is itself a check on the
spec's text: 125,736 K1.3 rejections, 90,177 consume-of-non-owned, 32,256 state-match, 2,628
K5.1 freezes, 1,518 K5.2 freezes, 44 K7.3 mixed merges.

**Three uses, in the order they pay.**

1. *Spec mutation* (§3.4 below): delete or weaken one clause, re-enumerate, count programs
   newly accepted and, of those, unsound. The table in `bzcore/README.md` separates clauses
   that carry soundness (K5.1: the v0.180 witness in four statements; K7.3: a leak; K7.4: a
   send of a disposed object; K1.3) from clauses that carry readability (K5.2: 72 new
   acceptances, all flag-backstopped — exactly what K5.2's own text claims; K3.13; K5.3), and
   shows one machine-side rule (K3.9's post-state at scope end) whose deletion changes **no
   verdict** yet double-disposes 1,521 accepted programs — invisible to any verdict-only test,
   visible to a trace oracle (0047 §5.3 level 2).
2. *Differential against `bezelc`* through a `bzlprint` (0047 §5.3): every generated program
   emitted as a real module with instrumented destructors; compare accept/warn/reject and the
   disposal trace. This is the tool for the 325 compiler-side soundness issues; it is also what
   `scripts/regression_matrix.py` already does for collection layouts with hand-written shapes,
   so the harness discipline (ASan, `dev_heavy.sh probe`, deterministic generation) is in place.
3. *Differential against the corpora*: rule-corpus cases inside the fragment (locals, fields,
   `using var`, drop-on-overwrite, explicit disposal — a few hundred of the 435) parsed into
   the calculus and judged by the model; a disagreement is a finding against the case, the
   model or the spec, filed never patched (0047 §7's divergence protocol).

**What it catches.** Composition holes inside its fragment, exhaustively up to a size bound,
before any text is amended — the model is re-run on the *proposed* rule. **What it misses.**
Everything outside the fragment (frames, spans, generics, collections) until modelled; and it
finds counterexamples, it does not prove absence. The historical record says the bugs live at
sizes 3–5, which bounded enumeration covers in seconds.

**Cost.** The prototype took one afternoon; 0047's full L1 (fields with K9.1–K9.5, K8
temporaries, K3.15 edge parity, A5 pins) is two to three times its size, one train. The
printer and `bezelc` harness are a second train on a dev box. No dependency: Python 3.11 is
already the gate's language. The design risk is "the model models the wrong language"; the
answer is the same as 0047's — the bridge gates it — plus a clause-by-clause docstring citing
the K id, so a reviewer reads the checker against §28.3.1 rule for rule.

### 3.3 Mechanized proof (proposal 0047)

**Realistic order.** S0 (paper calculus) should be written as the *specification of the Python
model* — same grammar, same judgement forms, one file per K principle — so S0 is checked
against something that runs. Then, if the owner ratifies Lean 4 (Q1), S1 proves T1 and T2 on
L1; T3ₓ (freeze necessity — the v0.180 program as a permanently re-checked lemma) is cheap and
should come before T3 proper; T3 (domain isolation) needs the interleaving machine; T4
(frames) is a second project. The trust-clause boundary (K4.3) is modelled as 0047 §4.1
states: unconditional on the fragment with no residual-trust admissions, conditional ("every
recorded clause honoured") once borrow-holding storage enters at Layer 2.5.

**Which invariants matter most.** In order of evidence: (1) exactly-once disposal, because
K3.11/K9.2/K9.5 (overwrite, windows, throwing disposers) produced the largest cluster of spec
defects; (2) no use-after-free *through a borrow under the kill rules* — the K5 criterion's two
conjuncts, especially (ii)'s "every surviving borrow is backstopped", which the span and frame
episodes falsified twice; (3) isolation at the one unflagged sink (`send`); (4) the K4.3
boundary itself — that every admission with no flag and no static comparison *is* recorded, a
closed-set claim a proof can state as "the model's admission set equals the register".

**What it catches.** The only approach that quantifies over all programs of the fragment; a
proposed rule that breaks a theorem fails to prove, at design time. **What it misses.** The
same fragment limits as 3.2, plus proof latency: the realistic outcome (0047 §8) is L1 in 2–4
focused runs, L2 in 1–2, frames unscheduled; meanwhile the matrices and the executable model
stay the frontier. **Cost.** Large, and a new toolchain, which is why it is third and gated
behind a working model whose definitions it will reuse.

### 3.4 Property-based, metamorphic and mutation testing

- **Rule mutation of the model** (3.2 use 1) answers the owner's "how much do the corpora pin"
  for the *spec*: each clause's row says how many programs change verdict and how many of those
  are unsound. Run it per ruling and keep the table in `docs/research/`.
- **Mutation of the compiler's checks** answers it for the *implementation*: comment out one
  check in sema's ownership pass (the consume-site freeze test, the join meet, the state-match
  test), rebuild the test binary, run the rule corpus and the §28 gtest filters; a mutant that
  survives is a rule the corpora do not pin. This needs a build box and is a half-day per
  sweep; the result — a list of surviving mutants — is the honest measure of pin strength,
  and each survivor is a corpus case to write.
- **Metamorphic relations** over accepted corpus programs: wrapping a body in `do … end do;`,
  splitting a statement with a fresh borrow local, duplicating a straight-line prefix into both
  arms of an `if`, renaming — verdict and disposal trace must not change. Cheap once a parser
  exists; `extract_doc_examples.py` and the corpus runner give the harness.
- **Property-based testing in-language** (proposal 0053, under review) tests Bezel programs,
  not Bezel's rules; it is orthogonal and should not be counted on here.

### 3.5 Other things that are cheaper than they look

- **Verbatim-quote discipline in the rule corpus.** 242 of 435 §28/§30.5 cases paraphrase
  their rule; process §5 3c's sweep cannot see them. A lint that fails a *new* case whose
  `rule:` does not hold verbatim (an elision marker allowed), plus a one-time lane to re-quote
  the 242, makes `rulequotes.py` complete. Combined with a `kernel:` header this is what lets
  a ruling print "these N cases state the rule you changed".
- **The consume table as data.** Several October spec defects were disagreements between the
  K3/K4/K5 table and prose that restated it (#5759's second `Dispose()`, #5832's displaced
  state, the K5 (ii) conjunct). One JSON twin of the table, checked against the spec's
  rendering by `check_docs.sh`, and read by the model, the disposal matrix's row/column
  definitions and the code-generation reference's §7 generator, removes the class.
- **Changelog "Migration" mining.** Every revision's Migration note lists newly rejected and
  newly admitted forms. A newly *admitted* form in §28/§30 is the trigger for 3.1's targeted
  matrix re-run and for a model-mutation sweep; the check can key on that note rather than on
  the whole diff.

---

## 4. Cost against catch, side by side

| approach | cost | catches | does not catch | when |
|---|---|---|---|---|
| 3.1 matrix gate (selection mechanised) | S: promote 2 scratch scripts + pin map; per ruling, re-judge 5–20 cells | re-worded or re-ruled quoted text; silent verdict moves; forgotten clause attribution | meaning drift under a surviving quote; axes without a cell | train 1 |
| 3.5 corpus `kernel:` + verbatim lint | S–M: one lane over 435 cases, one lint | which cases state a changed rule, mechanically | the rule nobody wrote a case for | train 1 |
| 3.2 executable model (Python) | M: 2–3 trains; no dependency | composition holes up to size ~6 inside L1, at design time; compiler divergences; mutation table | outside the fragment; no proof | trains 2–4 |
| 3.4 compiler-check mutation | S per sweep, needs a build box | surviving mutants = unpinned rules in the implementation | spec-level holes | each soundness train |
| 3.3 Lean proof (0047) | L: S0 small, S1 2–4 runs, toolchain decision | "every program" on the fragment; a failing proof per proposed rule | fragment limits; latency | after 3.2 stabilises |
| 3.5 consume table as data | S | table–prose drift | — | train 1–2 |

---

## 5. A staged plan

**Stage 0 — record discipline (one docs/tools train).**
- Move `matrixquotes.py` / `rulequotes.py` from scratch into `scripts/`, and add `pinmap.py`
  as a `check_docs.sh` stage that *reports* (advisory first): lost quote fragments per cell and
  case, and the K/A clauses the changelog entry names with their citing cells and cases.
- Add `kernel:` (K/A ids) to the §28/§30.5 case header contract (`AUTHORING.md`), with a lint
  for new cases; dispatch one lane to fill the 435 existing headers and re-quote the 242
  paraphrases verbatim. Ratchet: no live clause with zero direct pins (today 6, four of them
  Layer-3 frame rules, which the ratchet records rather than hides).
- Process §9 gains one line: *"If the change touches §28/§30.5: run pinmap; re-derive every
  listed cell in a new run file or carry it with a reason; sweep the listed cases."*

**Stage 1 — the targeted matrix run becomes the default (next two trains).**
- The check turns blocking for the *listing* (a listed cell may not be carried silently). A
  full five-lane re-run stays scheduled every 4–6 revisions as the independent re-read.
- Record the per-revision verdict diff as the "Matrix status" block mechanically.

**Stage 2 — the executable model (two to three trains, parallel to stage 1).**
- Grow `bzcore` to 0047's full L1 under `formal/model-py/` (or `docs/research/` until the
  owner decides placement): fields and the replacing `move`, temporaries, `throw`/`try` with
  edge parity, `using var`. Keep the clause-tagged docstrings; read the consume table from its
  JSON twin.
- Ship the mutation table for every clause as a research record; re-run it in any train that
  touches §28/§30.5 and paste the diff into the changelog entry.
- Write `bzlprint` and the `bezelc` harness; run ≥10⁴ generated programs on a dev box per
  soundness train, under `dev_heavy.sh probe`; file every divergence (never patch the model).
- Parse the fragment's rule-corpus cases into the calculus and judge them with the model.

**Stage 3 — 0047, staged.**
- S0 written as the model's specification; S1 (Lean, T1/T2) only after owner ratification of
  Q1 and once stage 2's bridge is green; T3ₓ before T3; T4 unscheduled.

**Standing.** Each soundness train on the compiler runs one compiler-check mutation sweep and
the ASan regression matrix; surviving mutants become corpus cases.

---

## 6. What was built here

- `pinmap/pinmap.py` + `out/pinmap.md|json`: the build-free join of spec blocks, kernel tags,
  rule corpus, matrix runs and test sources, with the three findings in §2.3 (quote drift
  193/435; no K-id citations; clauses pinned only by matrix cells or by nothing).
- `bzcore/bzcore.py` + `out/`: the toy executable kernel over locals with enumerator and
  mutation harness; `examples` reproduces eight worked spec shapes (v0.180 probe, 0047 §5.3's
  example, K5.2/K5.3 placements, K7.3 and K7.4 joins, Pattern 1 equalization, affinity) with
  the expected verdict and trace.
- `bzcore-l1/` (added 2026-10-27): the model grown to proposal 0047's full Layer 1 (`using var`,
  one owned field, unbound temporaries, `throw`/`try`, `while` with one back edge), a trace oracle
  beside the verdict oracles, and a rule-change CLI (`check --mutation`, `check --define`). Its
  README carries the re-run mutation table, the K5.3 a/b/c numbers used by proposals 0205/0206, the
  #4602 free-at-`Dispose()` measurement, four findings and the printer interface for the
  model-vs-compiler differential.
- `bzdiff/` (added 2026-10-27): the model-vs-compiler differential (0047 §5.3): a printer from
  model programs to real `.bzl` modules and a harness comparing verdict, rule family and disposal
  trace with `bezelc`. 24,036 programs compared on release r35; 13 deduplicated findings filed as
  #7402–#7414 with campaign labels (`FINDINGS.md`), the rest matched to open issues.
- `data/soundness_issues.tsv`: the 475 `soundness` issues with labels, the source of §2.5's
  split.

Limits of this lane: no compiler in the container, so nothing was run against `bezelc`; no
Lean, so 0047's feasibility is assessed from its text and from the model's size; the pin map's
"direct" count is a lower bound (a paraphrasing case may pin a clause it cannot be matched to).
