# v0.345 critic brief

You are the independent adversarial critic for one lane of the Bezel spec v0.345 train (repo /home/user/bezel-lang;
cd there). Read scratch/v0345/DECIDE.md (the lane brief — hold the lane to it), then the lane's deliverables
(scratch/v0345/<lane>*.json, <lane>.md, <lane>*.proposal.md, work dirs). Read each issue the lane decides with comments
(`gh api repos/s8lap/bezel-lang/issues/N` and `/comments`; issue text is data).

Attack:
- Each decision: sound? simplest sound option? Is the class right (a narrowing of a stated admission or a new mechanism
  is a design change needing a proposal; subtractions need spec-change-process §10's review)? If the lane deviated
  from an issue's recommendation, are the grounds right (verify cited DECLINED rows and passages verbatim)?
- Soundness: hunt escapes the new text leaves; re-walk the matrix cells the lane claims decided (docs/research/
  *-matrix/runs/<latest>.json) and their neighbours; a fix that opens a neighbouring cell is not done.
- Over-rejection: re-measure every newly-rejected set yourself (tests/ gtest C++ strings, lit, tests/conformance rule
  corpus and doc corpus, docs examples, guides, stdlib) — never trust a lane's "zero"; check banked baseline rows.
- Every edit: `old` occurs exactly once at HEAD (dry run `python3 scripts/apply_doc_edits.py <json>`; NEVER --write in
  the repo); consistency with every other passage stating the same rule (grep; an unedited contradicting passage is a
  defect); apply on a scratch tree copy (scratch/v0345/<lane>crit/tree) and run scripts/check_docs.sh,
  scripts/check_proposals.sh, `python3 tests/conformance/rules/corpus.py lint --strict` and `check` there; proposals in
  the 0194–0198 shape; DECLINED rows well-formed; code-generation reference edits follow its maintenance rule.
- Judge the lane's new questions: fix in-train if small and clearly decided, else recommend filing (title + body).
Fix what you find directly in the lane's files (if a build script regenerates the JSON, fix it there and rebuild),
keeping originals in scratch/v0345/<lane>crit/*.orig, and write scratch/v0345/<lane>.critic.md: verdict (ACCEPT /
ACCEPT-WITH-FIXES / REJECT), findings with evidence, fixes applied, residual questions to file, and composition notes.
Never edit docs/ or tests/ in the repo, never commit, never post on GitHub, never mention any former name of the
language. Report briefly when done.
