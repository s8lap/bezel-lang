# v0.345 composition: seams resolved

Apply order (`apply_all.sh`): C1, C2, C3, E, F (lane JSONs, renumbered and sequenced by `apply_lanes.py` ->
`lanes_adjusted.json`; E from `scratch/v0345/Fwork/E.reanchored.json`), then `scratch/v0345/comp.json` (built by
`build_comp.py` against `base/`, the lanes-only tree), then `scratch/v0345/corpus_edits.json` (`build_corpus.py`, applied
by `apply_corpus.py`, which rebuilds the rule corpus's manifest and coverage matrix; `apply_corpus_repo.py` is the
landing's twin without the repo refusal). Base: `f6863e622` (= origin/main; its `docs/` equal `b4243d87f`, the lanes'
base). Tree contents: `docs/`, `scripts/`, `tests/conformance/`, `editors/`, `CLAUDE.md`, `README.md`; every tree is
`git init`-ed. Proposal files (written renumbered by `apply_lanes.py` into `comp_work/proposals/`, copied in by
`apply_all.sh`): `C1-0200.proposal.md` -> `0200-dispose-member-only-on-disposable-types.md`, `C1-0201.proposal.md` ->
`0201-no-method-reference-to-dispose.md`, `C2-0202.proposal.md` -> `0202-type-position-qualified-head-is-not-a-chain-head.md`,
`C3-0204.proposal.md` -> **`0203-range-literal-target-typing-and-pattern-constants.md`**, `F-0206.proposal.md` ->
**`0204-char-pattern-constants-are-chars.md`**. 111 lane edits -> 111 applied (1 re-anchored, none dropped, 6 renumbered);
15 composition edits; 5 corpus edits. `mktree.sh DIR [--lanes-only]` builds a tree; `validate.sh` builds `comptree`, runs
every build-free check, then simulates `finalize.py 0.344 0.345` on `fintree` (dummy numbers in `fin/nums.json`).
`head/` is `git archive f6863e622` (read by `rulequotes.py`, `tools/matrixquotes.py`, `tools/excorpus.py`).

## A. Lane-side and composition-side seams

| # | Seam | Resolution |
|---|---|---|
| S1 | `DECLINED.md` `rejected` group end: C2 (7), C3 (5), E (5) | E.json anchored on the group's last row tail + `### deferred`, which fails once C2's and C3's rows sit at the group end (`apply_doc_edits.py` then writes nothing of E). `Fwork/E.reanchored.json` (decision 1) anchors on the heading alone; `apply_lanes.py` asserts it equals E.json except that anchor, the rows identical. Rows land C2, C3, E. |
| S2 | `declined-at-proposal` group end: C1 (0200 ×3, 0201 ×3), C2 (0202 ×3), C3 (0203 ×6), F (0204 ×2) | All anchored on `### non-goal`; sequential insertion, lane order = number order. |
| S3 | README *Accepted* end: C1 (0200, 0201), C2 (0202), C3 (0203), F (0204) | All anchored on `\n\n## Graduated` (C3 without the trailing newline; same insertion point). comp.json moves all five to *Graduated* after 0198 (4-cell form; status flipped to `graduated-v0.345 — superseded — graduated v0.345 (accepted 2026-10-27 as a delegated ruling …, <summary>; landed with the v0.345 changelog entry)` in the proposal files and the rows alike; the lanes' "critique pending" statuses replaced). |
| S4 | **Renumbering** (decision 2) | `apply_lanes.py` maps, simultaneously, the whole token `0204` -> `0203` and `0206` -> `0204` (no letter, digit, `_` or `.` on either side) in every lane edit's `new`, every edit id and the five proposal files; no `old` contains either token (asserted). F's text names both (its "0204" is C3's proposal). Leftover grep on `fintree/docs` (`(?<![0-9A-Za-z_.])020[5-7](?![0-9A-Za-z])`): only the changelog entry's two sentences that describe the renumbering. Every `0203`/`0204` token outside the two proposal files (changelog 23, rationale §11.11 2, README 3, DECLINED 9) was read in context: each names the right proposal. trackers.json / questions.json: no `0206` (asserted). |
| S5 | README never-used numbers; DECLINED index (decision 3) | comp#readme-never-used lists 0179, 0180, 0187, 0188, 0190, 0191, 0193, 0195, 0199 (each reserved by a train and not needed) and says the v0.345 train's numbers are contiguous. comp#declined-footer: `count_declined.py` 1038 -> 1072 (562 · 136 · 318 · 7 · 49 · 0; +17 `rejected`, +17 `declined-at-proposal`; v0.341–v0.344 added none). |
| S6 | Pins / version headers | No lane moves one (asserted); `finalize.py` moves the four headers and the reference's three pin lines (the simulated run passes). |

## B. Shared paragraphs (re-read as composed)

`tools/shared_paras.py` (each edit's `new` located in `base/`, grouped by blank-line block) lists the blocks two lanes touch:

| Place | Edits | Reading |
|---|---|---|
| Appendix C.7 | C3 (integer-scrutinee bullet), F (char bullet) | **Fixed (decision 5):** C3's bullet ended "as a range pattern's bounds do over a `char` scrutinee", true but narrower than 0204's rule. comp#C7-align-0203-0204 makes it "as every pattern constant does over a `char` scrutinee (the bullet below)"; the same text in 0203's ▲ (comp#C7-align-0203-proposal), and 0204's composition note records it as applied. |
| Appendix C.1 | C2 (`GetType`/`ReferenceEquals`), C3 (range literal), F (value kinds and `IAsyncDisposable`) | Separate bullets. |
| Appendix C.3 | C1 (two disposal bullets), E (#7143 borrow store) | Separate bullets. comp#E-7143-appc-tag adds the binding's derivation (E's critique). |
| §2.7 rule 1 | C2 (#7077 "declared or inherited"), F item 6 (type parameter at its link) | Different sentences; consistent with C2's #7116 sentence in §11.6. |
| Code-generation reference §7.1 | C3 (0203 lines), F (0204 line) | C3's lines, then F's. |
| §28.6 *Leak prevention* | E (#7143 bullet) | E alone; comp#E-7143-tag: "(derives: K1.7 + K2.3; for a binding, *State match on reassignment*'s K1.7 + K3.11)", as E's critic suggested. |

Read across sections (no text fix needed): §8.7 (C1) with §28.12 (F item 9) — `IAsyncDisposable` now extends `IDisposable`, so a value held at it has `Dispose()` by 0200's own wording ("the interfaces `where T: IDisposable` admits"); §6.2 (F item 2), §6.6 (E #6902) and §16.1 (C3) — one constant-typing rule (`u += -1` has no fitting constant; `5U..10` / `-5..10U` typed as `5U + 10` / `-5 + 10U`); §22.8 — C2 (#6903, #6982) and E (#6895) edit different sentences; stdlib §9.6 — C1 (lead), E (Pad comment, *window members*), F (`AsSpan`/`From`) different paragraphs; §19.3 — C3 (four paragraphs) and F (items 3, 4) different paragraphs. Rationale §11.11: 0203's paragraph, then 0204's.

## C. Composition edits (`comp.json`, 15)

| id | Seam | Resolution |
|---|---|---|
| `comp#p0200-status` … `comp#p0204-status` | Graduation (decision 4) | The v0.340 form; summaries written fresh from the critiqued proposals (the lane statuses still said "critique pending"). |
| `comp#readme-accepted-out`, `comp#readme-graduated-in` | README | All five rows leave *Accepted* (21 left) and enter *Graduated* after 0198; tracking cells name the issues and the landing's trackers (0200: #7273 retitled; 0202: the `s2_7_0039` flip). |
| `comp#p0202-s2_7_0039` | **0202's measurement** | A §6 bullet: one banked rule case moves (below, §D). |
| `comp#C7-align-0203-0204`, `-proposal`, `comp#C7-align-0204-note` | Decision 5 | §B. |
| `comp#E-7143-tag`, `comp#E-7143-appc-tag` | E's critique note | §B. |
| `comp#readme-never-used`, `comp#declined-footer` | Decision 3 | §A S5. |

## D. Corpus (`corpus_edits.json`, 5)

`rulequotes.py head base` flagged 6 cases (`stale_base.txt`); after the edits 1 remains (`stale_after.txt`: `s2_7_0039`, deliberately), and `tools/refreshed_hold.py` confirms all 5 refreshed quotes hold verbatim.
- Banked (header only): `s11_6_0020` (§2.7 rule 1's chain continuation, C2/#7077; its header was a paraphrase, now an ellipsis quote of two verbatim segments), `s22_8_0009` (§22.8, C2/#6903; ellipsis over the inserted clause), `s28_2_0002` (§28.2's restatement, C1/0200).
- Unbanked: `s14_2_0032`, `s14_2_0035` (§14.2's `U := Object` sentence, F item 8). F's critic called them banked; neither has a `baseline.tsv` row at `f6863e622` (asserted in `build_corpus.py`). `s35_3_0009` is unaffected (E's critic moved the insert past its quoted parenthesis).
- **Not edited — a banked verdict flip the lanes missed:** `s2_7_0039` (#7085's case, `expect: warn`, banked `pass`): `using var i: Choice.Item := new Choice.Item();` inside `Outer`, which declares a nested `Choice` beside a module-level `Choice`. Both heads are type positions, so under 0202 point 2 they name the nested `Choice`: nothing warns and the program prints `2`. Found by the quote check (its header quotes the old *Chain head* row); `fold/scan_0202_typepos.py` finds no other program in the rule corpus, the doc corpus, gtest strings or lit with the shape. It is listed in the 0202 tracker (re-cut to `run`/`2`, re-banked, plus an expression-chain-head `warn` twin), in 0202's §6 (comp#p0202-s2_7_0039), its README row and the changelog entry.
- No rule-corpus case flips verdict under the v0.345 text otherwise (the lanes' and critics' sweeps); no case is retired.

Doc-example corpus (`tools/excorpus.py`, `tools/excorpus_detail.py`; not regenerated, #1380): 743 -> 745 blocks; new: `codegen_0086` (C1's §7.2 `Dispose` block), `codegen_0125` (C2's §7.8 member-store block); changed: `codegen_0015` (E #6778 `Console` comment), `codegen_0017` (F9 index line), `codegen_0046` (C3/F §7.1 `// WRONG` lines), `codegen_0089`->`0090` (C2 #6764 §7.3), `codegen_0121`->`0122` (C3 #7032 §7.8 line), `spec_0221` (F9 §28.12 declaration), `stdlib_0011` (F9 §2.10), `stdlib_0035` (E #7275 `Step` comment), `stdlib_0052` (C1 §4.5 `OwningArray`), `stdlib_0148` (§9.6). **Four committed cases are byte-equal to a changed block and change text at the next re-extraction** (verdicts hold; not hand-edited, per decision 8): `spec_0234` and `stdlib_0011` (`fragment compiles`; the declaration gains `: IDisposable`), `codegen_0073` (`error`/`diagnose`, = head block `codegen_0121`; gains C3's `if s is string? then … end;` line, which compiles — the block's other lines still diagnose), `codegen_0017` (`skip`).

## E. Matrix (not edited; for the next runs)

`tools/matrixquotes.py head comptree`: 12 v0.336 cells lose a fragment, verdicts unchanged.
- generics R1–R6/C4, R8–R10/C4: §14.2's "At `U := Object` the bound admits `Object` and every class type, the set a class bound names" (F item 8 keeps the class-only half).
- generics R3/C6, R7/C6: §12.7's "a nullable substitution of the target counting as its underlying type" (C3/#7034 restates it as "identical with nullability stripped from both").
- disposal R10/C12: §15.1.1's "the box's `Dispose()` being §28.9's no-op" (C1/0200; `exempt` stands on K1.6).
Recorded in entry.md.

## F. Facts that moved since the lanes drafted (on `main` at `f6863e622`)

- #7262's fix (`39c77c9bb`) checks `Step` for overflow and gives `char` the code-point domain: #7275's compiler side conforms (E said "wraps today"); #7262 is closed and gets a note only.
- #7260's narrowing fix (`be9652d53`) is on `main`: the function-value arm of 0200 conforms.
- #7025's fix (`b6ac93f7c`) is on `main`, not staged: 0201 reverses a landed admission (owned-class-receiver thunk).
- #7172 (`dad5747b5`) and #7168 (`c0c9e3f9d`) fixed `string.Insert`'s clamp and `Remove`'s start check; #7108 stays open with a note.

## G. Landing data

- `trackers.json` (`build_t.py`): 50 entries — 13 `create`, 20 `close`, 17 `comment`. The 23 decided issues: 20 close (`spec-question` / `spec-defect` / `needs-owner-decision` removed where carried; #7275 loses `needs-owner-decision`, its owner flag surfaced in the entry); #7273 (retitled; `spec-question` -> `spec-tracking`), #6772 and #7281 (`spec-question` -> `spec-tracking`) stay open as their own trackers. Notes on #6595, #5191, #7260, #7025 (closed), #7262 (closed), #3850, #7108, #6895, #6902, #7282, #6778, #7048, #6874, #7143 (closed). No body links a file on `main` (the train lands on a branch); proposals are named by path. Create bodies are acyclic (file_issues2's order simulated: C3-0203-impl no longer references F-0204-impl back).
- `questions.json` (`build_q.py`): 1 — F item 7 (an import alias at a chain head). Every other residual is folded by F or is a tracker.
- `post_labels.py [--write]`: applies every `remove`/`add` and the #7273 `retitle` (post_land.py reads none); dry run lists 27 operations.
- `nums.json` is written by `scratch/file_issues2.py scratch/v0345` at landing (`finalize.py` and `post_land.py` read it).
