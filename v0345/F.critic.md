# Lane F (fold-ins) — critic report, v0.345

**Verdict: ACCEPT-WITH-FIXES.** Every decision is sound and none moves a banked verdict. I fixed five defects in the text, all of them gaps or overclaims inside F's own edits, and corrected two measurement claims in F.md. Proposal 0206 is consistent with C3's 0204 and with §16/§19.3, and its reach is 0 under a wider scan than the lane ran.

Originals are kept in `scratch/v0345/Fcrit/*.orig` (`F.json`, `F.md`, `F-0206.proposal.md`, `build.py`). The fixes went into `Fwork/build.py`, which regenerates `F.json`: 19 edits, 19/19 clean at HEAD `933953a8e`.

## Composition (measured)

- Scratch tree: `scratch/v0345/Fcrit/tree`. It was `git init`-ed from `git archive HEAD docs scripts editors tests`, and the five proposals were copied in under their md-stated names (0200, 0201, 0202, 0204, 0206).
- Edits applied in order: C1 (24), C2 (25), C3 (24), E, F (19).
- **E.json as shipped fails** after C2 and C3 (`FAIL 18 [E-declined-rows] … OLD not found`), and `apply_doc_edits.py` then writes none of E. I confirmed this myself. `Fwork/E.reanchored.json` differs from E.json only in that row's `old`/`new` tail: it anchors on the bare `\n\n### \`deferred\`` group end, and the row content is identical. With it, all 111 edits apply clean.
- On the composed tree, every check passes:
  - `check_docs.sh` is OK. Its one `CITATION` advisory (`[spec] §4.12`) also appears on a HEAD-only worktree, so it is pre-existing.
  - `check_proposals.sh` is OK: 195 ↔ 195, accepted 26.
  - `corpus.py lint --strict` reports 0 problems, and `check` is up to date (3343 rows).
- Logs: `Fcrit/check_docs.log`, `Fcrit/check_proposals.log`.
- Composed order verified: C.7 (relational → C3's bullet → F's → record), the code-generation reference §7.1 (C3's lines, then F's), and rationale §11.11 (0204's paragraph, then 0206's).

## Findings and fixes

1. **Item 4 / 0206: the `char?` gap (fixed).**
   - The drafted sentence said "Over a `char` scrutinee". The paragraph's carry-over clause ("every position the two patterns take", "a scrutinee §7.4 has narrowed") covers relational and range patterns and narrowed scrutinees. It does not cover a *constant* pattern over an un-narrowed `char?`.
   - The proposal's self-critique claimed that case is covered, but no sentence says so. *Constants* says "nullable or not" for integers.
   - Fix: the spec sentence now reads "Over a `char` scrutinee — nullable or not, read at its underlying type, as an integer one is (*Constants*, above) —"; the C.7 bullet gains "nullable or not". The proposal's ▲ text, §5 and §11 are updated to match.
   - Consistency checked against 0204:
     - The integer rule (*Constants*, *Relational*) and the range rule, which already names `char`.
     - The pattern/expression split.
     - At `in`, the fallback to the literal's own type. This makes the expression `c in 65..90` a `Range<int>` test, exactly as with `b in -100..0` (§16.1, 0204), so F's statement that "the expression keeps its promotion" holds there too.
     - The DECLINED row 0204 option M (promoted comparison), which is distinct from 0206's char-specific option A row.

2. **Item 4: reach re-measured at 0.**
   - F's `measure.py` only finds scrutinees bound as `name: char`. That misses inferred bindings (`var ch := s[i]`, `:= 'x'`), `for ch in s` elements and indexed scrutinees.
   - My scans (`Fcrit/work/scan_char.py`, `scan_char2.py`) cover all of those over `tests/` gtest/lit, both corpora, `docs/*.md` (spec, stdlib, guides, Tour, code-generation reference) and the proposals. The two hits are an `int` indexer (`this_indexer_typing_2795`) and `case month of` in prose. Neither is a `char`, so reach is 0.
   - No banked row involves a `char` scrutinee with an integer constant (`s16_6_0006`–`0008` and `s19_3_0072` are `ulong`).

3. **Item 2 (§6.2): the paragraph left two constant pairs undecided and overclaimed the compiler (fixed).**
   - The drafted text decided an unsuffixed literal beside a suffixed one, and "a named `const` … a literal beside it converting first". It did not decide:
     - two **suffixed** literals (`5UL + -1L`);
     - two named constants beyond "on the same terms";
     - a literal that does not fit beside a named constant that does.
   - Fix: one stated order of trial. The unsuffixed literal goes first, then a suffixed literal, then a named `const`, with the left operand first between two of one kind; the first that fits converts. New examples: `U - 1` (pinned by the 6914 test), and `5UL + -1L` / `-1L + 5UL`, both `long`. The scope now names `int` beside `ulong` too, which the 6914 test pins.
   - Every banked or pinned row keeps its verdict: `s6_2_0003` and `0004`; `s5_1_0128`–`0131` (0130 and 0131 are still rejected); the `6914`/`6984` rows; and C3's `5U..10`, `-5..10U` and `0..3000000000U`.
   - **The compiler diverges in three unpinned corners** (`ExprTypecheck_Exprs.cpp` `adopt`/`isConstantSource`, read from code, unprobed):
     - `-1L + 5UL` is rejected (the left literal adopts with no fallback), while `5UL + -1L` is `long`.
     - `-1 + U` with `const U: uint := 5` is rejected, though `U` fits `int`.
     - A named constant wider than the type it adopts keeps the rejection, marked "deferred" (`:7429–7434`).
   - F.md said "Matches. No tracker". It now carries a tracker and a *Spec-ahead-of-impl* migration line, 0 sites.

4. **Item 3: "narrowed non-null throughout the arm's guard and body" contradicted §7.4's invalidation forms (fixed).** Those forms are a nullable store, a `ref`/`out` argument and a mutable capture, and F's own md says `s := null` clears the narrowing. The text now says the binding "enters the arm's guard and body narrowed non-null … and §7.4's invalidating forms apply to it as to any local". The ruling itself (written type, never retyped, by the generic `var x: T` case) is sound and is C3's recommendation.

5. **Item 9: enums and choices (fixed).** This is the "anything implementing `IAsyncDisposable` alone gains `Dispose` obligations" question.
   - **Classes** gain no obligation: the compiler-generated `Dispose()` every class has implements the inherited member (§28.12, stdlib §2.10, §28.2). A **record** listing `IAsyncDisposable` is caught by F's §9.7 clause.
   - **Enums** (§10.5: "An enum may implement one or more interfaces … must provide matching public members for every interface member") and **choices** (A.3 `ChoiceDecl … Inheritance?`) can list interfaces too. §8.7 says neither implements `IDisposable`.
   - After the extension, an enum or choice listing `IAsyncDisposable` would owe a `Dispose()` it cannot have, and the text no longer said what happens.
   - Fix: §28.12's new sentence now says only a class implements it, and that a record, enum or choice listing it (or an interface extending it) is a compile error at the declaration (§8.7, §9.7). A new Appendix C bullet, `F-9-spec-C-value-kind-disposable`, is placed after the enum-conformance bullet, an anchor no other lane touches.
   - Reach is 0: there is no `enum`/`record`/`choice` listing `IAsyncDisposable`, `IEnumerator<T>` or `IOwning*` in `tests/` or `docs/`. The compiler already rejects records and enums against `IDisposable`/`IAsyncDisposable` at a bound (`TypeResolution_Checks.cpp:12830–12843`, pinned by `record_enum_idisposable_constraint_4755_test.cpp`). Declaration-site rejection and choices are added to F's tracker.
   - Every other `IAsyncDisposable` passage checked; all consistent:
     - spec :152, :1182 (C1's 0200 rule), :2457 (the worked example, now true), §28.12 :7467–7508, :7830 (§29.7's synchronous fallback, now backed by a member), :8334, :11856;
     - stdlib :525–541, :2503/2532/2536 (async enumerator heads, now `IDisposable` transitively), :4686/4700 (`TaskScope`);
     - code-generation reference :806, :1062–1074, :1319, :2021; rationale :2173 (0145).
   - `TaskScope` gains no new escape. It is a class (it already had `Dispose()`, and `where T: IDisposable` already admitted it), and 0145 already rejects its interface-upcast owning positions.
   - Residual, not edited: §28.12 :7508 "When a class implements both `IDisposable` and `IAsyncDisposable`" is now a tautology. It was already redundant, since every class implements `IDisposable`, so it is harmless.

6. **Item 8: F's deviation from C1 is right and better grounded than F said.** The rule corpus **banks** `s14_2_0032` (`Rel<string, Object>`, expect `reject`, baseline `pass`) and `s14_2_0035` (a class at `U := Object`, `accept`, `pass`), and `type_param_bound_subst_5906_test.cpp` pins the rejection. C1's widening would have flipped a banked case. F.md said "unmeasured … corpus gap"; I corrected it. Both cases' `rule:` headers quote the replaced sentence ("the set a class bound names"). Verdicts are unchanged; listed for the corpus (banked: list only).

7. **Item 5: sound against §22.6's tiers.** §22.6 has exact versus converting only, co-equal among conversions, and treats constant narrowing as applicability only, never exact; F's clause mirrors the constant sentence. I reworded "a literal whose endpoints have no type of their own" (wrong for `0..3_000_000_000`, where `0` has a type) to "a literal its endpoints give no type (… an endpoint with no standalone type, §4.2)".

8. **Item 6: accepted, with one corner noted and not edited.** Inside `class Box<T>` a nested `class Inner` may declare its own nested type `T`, since §14.1 bars only a type's own parameters. At a chain head in `Inner`'s body, `Inner.T` is demoted ("last, after aliases"), so under F's clause `Box`'s undemoted parameter wins and the head is C2's #7116 error. At a type position, `Inner.T` wins. That agrees with the table's "a reachable type outranks a same-named … nested type" at a chain head. It falsifies F's "as `T` in a type position is" only in this corner, and the compiler (innermost scope first) likely picks `Inner.T`. 0 sites; worth a line in the #7116/0202 tracker.

9. **Item 1: verified.** §4.14's "The `AsSpan`/`AsReadOnlySpan` factories validate eagerly" is verbatim (stdlib :2910). It is consistent with E's *window members* paragraph and C1's §9.6 lead, and the compiler cites match.

10. **Item 7 (to file): agreed.** Folding it would put a third hand on 0202's Warning row. The ready-to-file text is fine.

11. **Shape.**
    - 0206 follows the 0194–0198 shape.
    - The README row and the two `declined-at-proposal` rows are well-formed; TABLE and TOKEN are clean.
    - The code-generation reference edit is one `// WRONG` line in the existing §7.1 block plus the §5.2 index line, with no prose pins (maintenance rule kept).
    - No history narration; `build.py`'s guard still runs.
    - No former name of the language appears.
    - F.md has no GitHub ruling comments. That is correct: none of F's items is a filed issue.

## Residual questions / trackers (for the coordinator)

- **§6.2 constant-search tracker** (`spec-tracking`, new; text in F.md item 2): `-1L + 5UL`, `-1 + U`, and a wider named constant.
- **0206's tracker:** add the `char?` row (`case c of 65` over `var c: char? := 'A'`, required: rejected).
- **IAsyncDisposable tracker:** add `enum E: IAsyncDisposable` and `choice C: IAsyncDisposable` at the declaration (required: rejected).
- **Composition, C3's text:** 0204's C.7 bullet ends "as a range pattern's bounds do over a `char` scrutinee". It is true, but narrower than 0206's rule. Suggested coordinator edit: "… as every pattern constant does over a `char` scrutinee (the bullet below)".
- **Composition:** use `Fwork/E.reanchored.json` in place of `E.json`, or re-anchor E's DECLINED row at composition.
- **Corpus (banked, list only):** refresh the rule quotes of `s14_2_0032` and `s14_2_0035`. Doc corpus: `stdlib_0011` and `spec_0234` re-extract (item 9).
- **Item 6 corner:** above; low priority.
