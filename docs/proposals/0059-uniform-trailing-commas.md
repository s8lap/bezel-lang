# Proposal 0059 — Uniform optional trailing commas

| | |
|---|---|
| **Status** | superseded — graduated v0.214 (critique-gated; the landed sweep is exhaustive over Appendix A's eighteen comma-list productions, per the critique's enumeration — the draft's list was short by seven) |
| **Tracking issue** | assigned at landing |
| **Target** | Spec §1.6 (one normative sentence), Appendix A (`","?` added to every comma-separated list production that lacks it: all eighteen comma-list productions: ArgList, ParamList, LambdaParams, LambdaDestructure, GenericParams, GenericArgs, GenericConstraints, PositionalFields, PositionalPatternList, EnumConstants, ChoiceCases, CasePayload, PolymorphicMember, ExternParamList, FunctionType, Inheritance, EnumInheritance (both alternatives)). Impl: parser. |
| **Written against** | spec v0.212 |
| **Origin** | 2026-07-21 sweep (consistency lens, survivor 1): four constructs allow the trailing comma, seven forbid it, and the boundary is unrecorded and unprincipled — enum constants and choice cases, the most one-per-line lists in the language, forbid what collection literals allow. |

## 1. The rule

**Every comma-separated list admits one optional trailing comma before its closer.** One sentence in §1.6; `","?` in every A-grammar list production. `|`-separated pattern alternatives are not comma lists and are untouched. The already-permitting constructs (collection literals, `with(…)`, case/when arms, catch arms) are unchanged.

## 2. Why uniform rather than a curated boundary

Any curated predicate ("element lists yes, declaration lists no") re-creates the memorization burden this deletes, and the current boundary is evidence curation fails (it landed backwards for the one-per-line lists). Uniformity is the only spelling with zero rules to remember. Cost: `f(a, b,)` becomes legal — mildly unusual in call position, harmless, and standard in the languages Bezel's users come from (Rust, Python, modern JS/TS, Swift).

## 3. Migration

Purely additive; no program changes meaning. Impl tracker filed at landing.
