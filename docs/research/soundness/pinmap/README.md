# pinmap — which kernel clauses and §28 / §30.5 blocks does anything pin?

Build-free, read-only join of four records the repository already keeps:

- the spec's §28 and §30.5 text, split into 113 blocks (h3/h4 headings and bold-head
  paragraphs, the 0046 audit's granularity), each with the K/A ids its `derives:` / `axiom`
  tags name, and the 80 kernel clauses and axioms of §28.3.1–§28.3.2;
- the rule corpus (`tests/conformance/rules`): the 435 cases in §28 / §30.5, their `rule:`
  quotes, sections and banked verdicts;
- the latest run of each completeness matrix (`docs/research/*-matrix/runs`): 464 cells and
  routes with the K/A ids and sections they cite;
- the gtest and lit sources under `tests/`: per-section citation counts (coarse, since those
  tests are pinned to issues, not rules).

```
python3 -I docs/research/soundness/pinmap/pinmap.py        # writes out/pinmap.md and out/pinmap.json
```

A case pins a clause **directly** when its `rule:` quote holds verbatim (the trains'
normalisation: links, emphasis and code ticks stripped) inside a block that derives from the
clause, or inside the clause's own text; it pins **by section** when it merely cites the
section. The report's tables: A — per clause; B — substantive blocks nothing quotes; C — per
section.

## What it found at v0.344

- **Quote drift.** 193 of the 435 §28 / §30.5 cases carry a `rule:` that holds verbatim in the
  current spec; 242 are paraphrases (whole corpus: 1,099 of 3,336). A paraphrase cannot be
  checked mechanically against a rule change, so the trains' `rulequotes.py` sweep (which this
  tool reuses) sees only the verbatim half.
- **No case cites a kernel id.** Clause-level pinning is therefore inferred through block
  tags, and only 56 of the 113 blocks carry a `derives:` tag.
- **Clauses with no direct corpus case and no matrix cell:** K1.2, K1.4, K3.6, K4 (head),
  K5.4, K5.8 — the Pattern A chaining pair (K3.6 / K5.4) and the guard-interior freeze (K5.8)
  are 0047's Layer 3, where nothing executable exists yet.
- **Clauses pinned only by matrix cells:** 44 of the 80, among them every K5, K7, K8 and K9
  sub-clause — the consume-site freeze, the join and the field rules are held by the matrices'
  quoted judgements alone, not by one executable case each.
- **68 of 111 substantive blocks (≥ 40 words) have no directly-quoting corpus case**, the
  §28.3.4 producers table, the §28.3.5 announcement table and all of §28.3.11 among them.

The numbers are a lower bound on coverage (a paraphrasing case may well pin the clause) and
an upper bound on what a *mechanical* gate could see today. That gap is the recommendation's
first target: a `kernel:` header on each §28 / §30.5 case and a verbatim-quote discipline make
the map exact, and then "a ruling touched K5.3 — these 4 cells and these 0 cases must be
re-derived" is a script, not a lane briefing.
