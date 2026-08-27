# 0121 — Subtraction batch, 2026-08: four removals, one recorded keep, two ghost rulings

**Status:** under-review — the output of the [2026-08 subtraction review](subtraction-review-2026-08.md) (process §10: the pass mints no removal; this proposal is its docket for the owner queue). Every claim below survived an adversarial verification lane that refuted three sibling candidates; the greps and record-searches are in the review record.
**Tracking issue:** [#3409](https://github.com/s8lap/bezel-lang/issues/3409)
**Target module(s):** stdlib §2.2 (`Console`), §5 (LINQ), §2.1 (`Math`), §6.12 (`Parallel`); rationale §14.4 (one paragraph); conformance corpus per item. **Grammar untouched by every item.**
**Written against:** v0.283

## 1. Removals proposed (ranked by cleanliness; each is a deletion-shaped change owing the §6 corpus accounting given inline)

**R1 — `Console.Clear()`.** Zero hits repo-wide (`grep -rn "Console\.Clear" .` = 0), zero record (rationale, `DECLINED.md`, changelog, all proposals), zero implementation (no sema arm, no codegen arm, no runtime entry). The cleanest possible removal: one stdlib line, no corpus contact. A terminal-control surface, if ever wanted, arrives with a real terminal module and a motivating program.

**R2 — LINQ `OfType<U>()`.** Zero record anywhere; zero usage outside its own two test assertions. Edit cost stated honestly: Guide 05 line 254 and stdlib §5's own prose mention it; two conformance cases exist and are both **skip-classed** (`codegen_0029`, `stdlib_0092` — no exercised case converts); the v0.278 "42 member names" enumeration becomes 41 (three carrier documents). Independent of this proposal's fate, its sema arm accepts an **unrelated `U`** with no check — filed as a soundness defect; if R2 is accepted the defect dies with the member, which is itself a small argument for R2.

**R3 — the `Math` transcendental block** (`Sin`/`Cos`/`Tan`/`Asin`/`Acos`/`Atan`/`Atan2`/`Sinh`/`Cosh`/`Tanh`/`Exp`/`Log`/`Log2`/`Log10`/`E` and companions — 18 names): zero usage member by member; `Fmod`/`IEEERemainder` stay (record-defended). One prior mention must be quoted past, not around: proposal 0042 §305 enumerates the block by name — as a **purity-annotation batch**, not a keep-defense. Removal must also edit the compiler's `Math` accept-list in the same landing, or the docs would assert an absence the compiler contradicts (process §5 item 3b). Corpus: zero exercised cases. *The counterargument, stated:* transcendentals are a services-language table-stakes claim; if the owner weighs that forward bet over present usage, the alternative is a **record** (one rationale sentence: kept on the table-stakes ground, re-measured each pass).

**R4 — `Parallel.Reduce`** *(the weakest; presented as a genuine choice).* Zero call sites; the whole of §6.12 is unimplemented in `src/`. But the keep has real recorded weight: rationale §12.9 carries a Reduce-specific design ruling (seed-threading declined — "how PLINQ earned its documentation warts") and graduated proposal 0032 §5.3 is a worked program using it; removal strands deferral mentions in three documents plus 0045. **Option A:** remove `Reduce` (two overload rows), accounting for the strandings. **Option B (recommended):** remove nothing; give all of §6.12 the honesty machinery it lacks — a spec-ahead-of-impl tracking issue and an in-doc unimplemented marker, the #3331/DangerousPointer standard — and re-ask at v0.303 with the deferral's age as evidence.

## 2. The recorded keep (lands with whichever revision graduates this)

**Bitwise operator overloadability (`& | ^ << >>`, §25.1) — keep, and write it down.** Zero clients, but removal is grammar-touching (Appendix A's shared `OpToken`/`IfaceStaticOperator` productions), would strand the compound-assignment desugar for `&=`/`|=`/`^=`/`<<=`/`>>=`, and would leave §14.4's `~` keep incoherent (its ground is "bit-pattern inversion **or set-complement operation**" — a `BitMask` able to complement but not union). One paragraph extends rationale §14.4, quoting proposal 0078 §94 so the record does not contradict that graduated ruling. Asymmetric costs, stated: record ≈ one paragraph; remove ≈ a design-change revision touching five grammar alternatives plus a deny-list-shaped `src/` edit that *adds* code.

## 3. Ghost rulings (defects filed at commit; the ruling itself is the owner's)

**G1 — `Math.Hypot`, `Math.Cbrt`, `Math.PI`: implemented but undocumented,** and worse — the code-generation reference line 1360 **asserts `Hypot` does not exist** while sema accepts and codegen lowers it. Per process §5 item 3b this is an impl-ahead-of-spec delta; the ruling is *document or remove from the compiler* (if R3 is accepted, remove-with-R3 is the consistent answer; if R3 is declined, document all three).

**G2 — the bit-tier `TryWrite*` forms have zero `src/` presence while the byte tier is fully implemented** — §4.14's two Try-write families are in opposite implementation states and neither doc says so. The fix is honesty machinery (a tracking issue + an in-doc marker), not removal — the byte tier's recorded write-side defense (stdlib §4.14) covers the bit tier's design.

## 4. Explicitly not proposed (verified keeps — so this batch's silence is not re-read as endorsement-by-omission)

`TryWrite*` byte tier, `StringBuilder.AppendFormat`, `Interop.CodeAddress<F>` (all: recorded defenses the sweep initially missed — see the review record); `DangerousPointer` (kept with a v0.303 expiry); `Convert` and the §2 `*OrNull` tier (kept on recorded rulings, measured hollow, re-ask docketed); every axiom (zero lost clients); the tombstone stubs (anchor-stability keep holds).
