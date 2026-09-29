# 0054 — Traceability conventions: contract-to-code citations

**Status:** accepted — accepted (phase 1; 2026-07-24, delegated ruling #2041 — S0–S2 accepted, phase 2/S3 deferred; the five §8 open questions ruled and the `guide3` id-scheme currency fix recorded in #2041's record)
**Tracking issue:** [#2041](https://github.com/s8lap/bezel-lang/issues/2041)
**Target:** tooling + convention (phase 1: zero normative change — rides spec §34's preserved-verbatim rule; phase 2, optional: a one-sentence addition to §34's recognized-tag list). Phase 1 spans implementation stages S0–S2 (§6); phase 2 is S3.
**Written against:** spec v0.197 (stdlib/rationale at the same revision)
**Currency note:** Refreshed 2026-09-17 to v0.306: §2.2's id-scheme row generalized from `guide1` / `guide2` ("the two guides") to `guide1` … `guide7` — the guide set is seven documents (`Bezel_Guide_01_*`–`Bezel_Guide_07_*`), and the `guide3` correction was already recorded in #2041's ruling but never applied to the table. Otherwise re-verified clean at v0.306: §34's preserved-verbatim rule, its recognized-tag list and rationale §18's attribute deferral all still read as cited. No tier-2 flag. Written-against pin unchanged.

## 0. Summary

A single, tool-readable convention for saying *what a piece of code or a test vouches for*: a `<satisfies ref="…"/>` documentation-comment element on Bezel declarations, a defined id scheme covering every citable target the project has (spec sections, contract clauses, proposals, issues, free-form requirements), and a `bezel trace-coverage` verb that computes coverage both ways — which targets have implementing declarations and citing tests, and which citations point at nothing. The repo already practices traceability informally in three surfaces (`// per §28.3` comments, `BEZEL_TEST(area, spec_1_5_example_3)` test names, and the guides' §-citations); this proposal makes the practice machine-checkable without changing what the language means: per §34, the compiler *never* interprets documentation-tag content, and that invariant is load-bearing here — the element can never become semantics smuggled through comments.

The regeneration framing (future-directions §0, P14): when implementations are cheap to rebuild, the audit trail — *what was rebuilt, against which contract, and what vouches for it* — is how a human reviews the result. Traceability is that trail's index.

## 1. Problem and evidence

The project already runs on citations; none of them are machine-checked end to end:

- **Code.** CLAUDE.md mandates the comment discipline "cite the spec section any non-obvious code derives from, in the form `// per §17.4`". These citations rot silently: a section renumbering or a rule's supersession leaves the comment pointing at the wrong authority, and nothing flags it. (`scripts/check_docs.sh` validates anchors *within* the docs — spec-change-process.md §3 — but no tool validates citations *from code into* the docs.)
- **Tests.** `tests/README.md` requires each test to name its spec section and example (`BEZEL_TEST(lexer_string_interpolation, spec_1_5_example_3)`). The convention is followed but unindexed: nobody can ask "which tests derive from §28.8.1?" or, more importantly, "which normative sections have *no* deriving test?"
- **Contracts (forward-looking).** Proposal 0043 puts behavioral clauses in signatures and 0044 carries them — with a precise identity, the contract record — in the artifact interface. Once clauses exist, "which test exercises this `ensures`?" is a question with a well-defined answer and no way to record it.
- **Doc comments.** Spec §34 defines the vehicle and, critically, its safety property: documentation comments use XML element syntax; recognized tags are preserved in output metadata for documentation generators; and *"Unrecognized tags are preserved verbatim in the same metadata: recognition affects only how documentation tooling may render a tag, never compilation."* A new element therefore needs **no language change** to be carried, extracted, and indexed — the metadata pipeline already transports it.

The gap is not a missing mechanism but a missing *convention plus index*: three citation surfaces, no shared id scheme, no coverage computation, no rot detection.

## 2. The design

### 2.1 The element

One self-closing documentation element, allowed on any declaration a documentation comment may precede (§34: "meaningful only immediately before a declaration"), repeatable:

```bezel
/// <summary>Withdraws an amount, never overdrawing.</summary>
/// <satisfies ref="spec:28.3"/>
/// <satisfies ref="contract:Banking.Account.Withdraw/ensures[1]"/>
/// <satisfies ref="req:banking-reqs.md#R-041"/>
public method Withdraw(amount: int): int
  ...
end Withdraw;
```

- `ref` is the only attribute in phase 1. An optional free-text `note` attribute is reserved (rendered by doc tooling, ignored by the coverage tool).
- Multiple elements accumulate; order is insignificant.
- The element states a claim by the *author*, not a fact checked by the compiler. The compiler's involvement ends at §34 preservation. The claim is audited by the coverage tool (§2.4) and by review — exactly the standing of a `// per §` comment today, minus the rot.

### 2.2 The id scheme

One grammar for every citable target, `scheme ":" body`:

| Scheme | Form | Resolves against | Example |
|---|---|---|---|
| `spec` | section number, or `#`-anchor | `Bezel_Language_Spec.md` headings/anchors | `spec:28.8.1`, `spec:#sec-consume-site-freeze` |
| `stdlib` | section number or anchor | `Bezel_Standard_Library.md` | `stdlib:4.8` |
| `rationale` | section number or anchor | `Bezel_Design_Rationale.md` | `rationale:5.23` |
| `guide1` … `guide7` | section number | the seven guides (`Bezel_Guide_01_*`–`Bezel_Guide_07_*`) | `guide2:5` |
| `proposal` | proposal number | `docs/proposals/NNNN-*.md` | `proposal:0043` |
| `issue` | issue number | the GitHub tracker | `issue:2030` |
| `contract` | member path `/` kind `[` 1-based index `]` | 0044's per-member contract records | `contract:Banking.Account.Withdraw/ensures[1]` |
| `req` | file `#` id | a free-form requirements file in the repo | `req:banking-reqs.md#R-041` |

Notes, each a decision:

- **Section numbers, not titles.** Titles get edited; numbers are what the docs' own cross-references use. Anchors (`#sec-…`) are accepted as the rename-stable alternative — they are what `check_docs.sh` already validates, so resolution reuses that logic rather than reimplementing it.
- **`contract:` addressing aligns with 0044.** 0044's contract records are carried *per member, in declaration order*; the address is the member's module-qualified path, the clause kind, and the 1-based position of the clause *within its kind* in declaration order. Renaming a parameter does not move an address (0044's canonical text positionalizes parameter references); inserting a clause above an existing one does — the same fragility as any positional scheme, accepted because clauses have no names and inventing label syntax for them is a language change this proposal refuses to make (§7 Q2 records the labeled alternative).
- **`req:` is deliberately unstructured.** A requirements registry is a Markdown file with stable ids in headings or list items (`#R-041`). The tool resolves the file and greps the id; the format war is not fought here.
- **Pinning.** Any id may append `@vN.NNN` (`spec:28.3@v0.197`) to pin the revision cited, mirroring CLAUDE.md's citation guidance ("cite the section *and* the revision"). Unpinned ids resolve against the current docs; pinned ids additionally let the tool flag *stale pins* (target text changed since the pinned revision — resolved via git history) without failing them.

### 2.3 The three indexed surfaces

The coverage tool reads citations from all three surfaces the repo actually uses; only the first is new syntax:

1. **Bezel `<satisfies>` elements** — extracted from §34 output metadata when compiled artifacts are available, or by direct source scan (the tool must not require a build).
2. **`// per §X.Y` comments in the implementation** (C++ today, per CLAUDE.md's discipline) — indexed read-only by pattern; each is treated as `spec:X.Y` with an *implicit, legacy* marker. No rewrite of the codebase is proposed; the tool simply stops these citations from rotting silently.
3. **Test names** — `BEZEL_TEST(area, spec_1_5_example_3)` per tests/README.md, parsed as `spec:1.5` (example index retained as metadata); lit-track files' header comments likewise. This is what makes "which sections lack a deriving test?" answerable with zero test churn.

### 2.4 The tool: `bezel trace-coverage`

A driver verb (0040 integration; standalone script until the driver lands):

- **Inputs:** the repo (sources, tests, docs), optionally built artifacts (for metadata-extracted elements and 0044 contract records).
- **Resolution pass:** every citation resolved; failures classified — *dangling* (target does not exist: the §-anchor missing, the contract clause index out of range, the issue closed-as-not-planned), *stale pin* (§2.2). Anchor resolution reuses `check_docs.sh`'s logic (spec-change-process.md §3) rather than duplicating it.
- **Coverage pass**, both directions:
  - per target (a spec section, a contract clause, a requirement id): the declarations citing it and the tests citing it — and the *uncited* lists: normative sections with no deriving test, public API with no citation, 0044-carried clauses no test claims to exercise;
  - per citation: what it resolves to.
- **Output:** human report and `--json`; the JSON vocabulary coordinates with proposal 0052 (drafted in parallel) — both tools emit spec-anchor references and should share the reference shape.
- **Exit codes:** 0 clean, 1 dangling citations found, 2 tool error. Coverage *gaps* (uncited targets) never fail the run in phase 1 — see §2.5.

### 2.5 CI posture

Advisory first, narrow gates later, in this order and each behind its own decision:

1. **Phase 1 (with the tool):** `trace-coverage` runs in CI in advisory mode — CI invokes it as a report and does not gate on its exit status (the §2.4 exit codes exist for direct and scripted use); *dangling* citations are surfaced in the summary but do not fail the build until option A is adopted.
2. **Recorded option A:** fail CI on dangling citations (rot becomes a build error — cheap, low-noise, recommended once the initial sweep is clean).
3. **Recorded option B (not recommended yet):** require a `<satisfies>` on new public stdlib API. Deferred: value unproven, and a mandated tag invites cargo-cult citations — the failure mode `req:` registries exhibit in every compliance-driven codebase. Revisit with evidence.

## 3. Grammar delta

None in phase 1. The element is an unrecognized tag under §34's rule — preserved verbatim in output metadata, never interpreted; documentation comments' syntax is untouched.

**Phase 2 (optional, separable; implementation stage S3):** add `<satisfies>` to §34's recognized list — one sentence in the tag enumeration. The *only* effect, per §34's own invariant, is that documentation tooling may render it (e.g., as a "Satisfies: §28.3" line in generated docs). Compilation is unaffected in both phases; there is deliberately no phase at which this element gains semantics.

## 4. Interactions

- **Spec §34.** The design's foundation and its guardrail: "recognition affects only how documentation tooling may render a tag, never compilation" is quoted here as a *normative ceiling* on this proposal's future — any follow-up that wants the compiler to act on `<satisfies>` is out of scope by construction and must arrive as its own language proposal.
- **Proposal 0044 (contract carriage).** `contract:` ids resolve against 0044's per-member, declaration-ordered contract records; the addressing is defined in §2.2 to be derivable from the artifact interface alone. Until 0044 lands, `contract:` ids resolve by parsing 0043 clause syntax from source (same order), so the scheme is usable from 0043 stage 1.
- **Proposal 0043 (contracts).** The bridge target: a test citing `contract:…/ensures[1]` is the recorded answer to "what exercises this clause?"; 0053's contract-derived properties (its §8) can emit the citation automatically when they are generated.
- **Proposal 0052 (diagnostics; drafted in parallel).** Shared spec-anchor reference shape in JSON output; `--explain`'s spec anchors and this tool's `spec:` resolution should be one implementation.
- **Proposal 0022/0053 (tests).** No change to either; test-name parsing (§2.3) is read-only. If 0022's discovery lands a `test` modifier, `<satisfies>` on a Bezel test method becomes the first-class test-side citation and the name-encoding becomes legacy — the tool reads both.
- **`scripts/check_docs.sh` / spec-change-process.md.** Resolution logic reused; conversely, a docs revision that renumbers sections gets, for free, a list of every in-repo citation the renumbering breaks — a gap the process document's hazard list currently covers only for intra-docs references.
- **Rationale §18 (attributes deferred).** Respected, and the deferral is part of this design's case: an attribute system remains the eventual first-class vehicle for machine-read annotations; `<satisfies>` deliberately lives in the documentation layer precisely so that nothing here preempts, constrains, or smuggles in that future design. Migration to attributes, if they ever land, is mechanical (one doc element → one attribute).
- **CLAUDE.md comment discipline.** Unchanged; indexed. The tool gives the existing rule its missing enforcement half.

## 5. Migration and compatibility

Nothing migrates. Phase 1 adds a convention and a tool; every existing citation surface keeps working and gains indexing. Adopting `<satisfies>` in the stdlib docs-comments is an incremental sweep with no deadline; the initial deliverable is the tool running over the *existing* `// per §` and test-name surfaces, which requires zero edits to anything.

## 6. Staging

- **S0 — the id scheme + resolver:** the §2.2 grammar, resolution against docs anchors/sections (reusing `check_docs.sh` logic), the dangling/stale classification. Deliverable: a script; CI report wired.
- **S1 — the three-surface indexer + coverage:** §2.3 scanners, both-direction coverage report, `--json`. Deliverable: `bezel trace-coverage` (or standalone until 0040).
- **S2 — contract ids:** `contract:` resolution from source (0043) and from artifact interfaces (0044) when present.
- **S3 — optional §34 recognized-tag row** and doc-generator rendering; CI option A (fail on dangling) if the owner adopts it.

## 7. Alternatives considered and declined

- **An attribute system now.** Declined without needing new argument: rationale §18 already records why attributes wait for their consumers; a citation tag has exactly one consumer (this tool) and needs none of an attribute system's machinery (typed parameters, runtime query, inheritance semantics).
- **Naming conventions only** (encode everything in identifiers, as tests do today). Works for tests because the harness owns the namespace; fails for declarations — mangling spec citations into API names is obviously absurd, which is why the code surface grew `// per §` comments instead. The convention-only status quo is precisely the unindexed state §1 describes.
- **A sidecar registry file** (central `traceability.toml` mapping ids to code locations). Declined: the citation belongs next to the thing it describes or it rots faster, not slower — the sidecar decouples the two edits every rename. (The `req:` registry is not a counterexample: requirements have no code location to sit next to.)
- **Compiler-checked citations** (make `bezelc` resolve `ref`s). Declined on §34's own invariant — documentation content never affects compilation — and on layering: the docs are not an input of the compiler, and making them one couples every build to the docs tree. Checking is the tool's job, in CI, where the docs are present.

## 8. Open questions / owner decisions

1. **Adopt the element name `satisfies`?** Alternatives: `implements` (collides conceptually with interface implementation), `derives` (test-flavored), `cites` (weakest claim). `satisfies` states the intended direction of obligation; recommended.
2. **Clause addressing: positional (`ensures[1]`) forever, or reserve labeled clauses?** Positional is fragile under clause insertion (§2.2). If 0043 ever grows clause labels (`ensures @nonneg …` — a language change, not proposed), `contract:` gains a label form. Decide only that positional is acceptable *now*; recommended yes.
3. **CI option A (fail on dangling) — adopt after the initial sweep?** Recommended yes; it is the cheapest rot-prevention on offer.
4. **Should `trace-coverage` also parse the *changelog* for supersession** (flag citations to sections whose rule was superseded even if the anchor still resolves)? Valuable but requires machine-readable supersession records the changelog does not carry; recorded as a future option, not designed here.
5. **Tool home before 0040 lands:** `scripts/trace_coverage.sh` (repo-local, like `check_docs.sh`) vs waiting for the driver. Recommended: script now, verb later — the S0 resolver is useful the week it exists.

## 9. Relationship to the P14 sketch

Faithful, with two deepenings and one correction. Deepened: the id scheme now covers contract clauses with 0044-aligned addressing, and the indexer covers the two *existing* citation surfaces (code comments, test names) rather than only the new element — making the tool useful before any new tag is written. Corrected, twice: the sketch's spelling was the at-form `/// @satisfies <id>`, replaced here by the XML element `/// <satisfies ref="…"/>` — a load-bearing substitution, because §34's preserved-verbatim rule covers XML *tags*, so only the element form rides the existing metadata pipeline with no language change (an at-form would have needed one). And the sketch said "no language change" unconditionally; precisely true only for phase 1 — the phase-2 recognized-tag addition is real spec text and is therefore staged, optional, and separable (S3).
