# Proposals syntax-currency sweep — 2026-07-28

*Supporting record (not a numbered proposal). Owner-directed 2026-07-28, prompted by an owner-spotted pre-v0.208 `case … end` expression in a fresh proposal's quotation of 0002. This record supersedes the **syntax** dimension of [`proposals-currency-2026-07-15.md`](proposals-currency-2026-07-15.md) — that report was compiled against v0.186–v0.194 and predates every retired-form class below; its per-proposal graduation-note matrix otherwise stands.*

**What was done.** Every non-superseded numbered proposal in the 0001–0061 range (36 files: 15 fixed + 19 clean + 2 flagged) plus the recent 0068–0074 set and the three exploration/comparison records (10 files, expected clean) was swept — with [`future-directions-2026-07-18.md`](future-directions-2026-07-18.md)'s contract clauses folded in at verification — for the retired-form classes the v0.187–v0.222 revisions created, and modernized **in place — spellings only, never semantics** (the 0004 refresh precedent). Each edited file carries a one-line `Currency note` in its header; written-against pins are unchanged (the pin records what the design was *checked* against; the note records that the *spellings* are v0.228). Deliberate historical exhibits (changelog quotations, declined-alternative displays) were left as written.

## The retired-form inventory swept

| Class | Retired form | Current form | Since |
|---|---|---|---|
| Lambdas | `(params) => body`, `x => body`, `async (…) =>` | `lambda(params) => body`, `async lambda(…) =>` | v0.217 |
| Expression `case`/`when` | `… case E of arms end` in expression position | `(case E of arm, …)` paren atom, no `end` | v0.208 |
| Conditional | postfix `a if c else b`; unparenthesized expression `if` | `(if c then a elif c2 then b else d)` | v0.208 |
| Record update | `expr with N := v, … end` | `expr with(N := v, …)` | v0.208 |
| Contract clauses | `requires E;` / `ensures E;` / `invariant E;` | `requires(E)` / `ensures(E)` / `invariant(E);` | v0.207 |
| Object initializers | `new T(args) with … end` on classes | removed (bind-then-assign) | v0.188 |
| Stale APIs | `ReadOnlyAsciiByteSpan`/`AsAsciiByteSpan` (v0.188); `event` members (v0.187); `DisposeElements` (v0.213); `Weak<T>.TryUpgrade(out…)`, Owning{Stack,Queue} `Add`/`Remove`/`IOwningCollection`, `Array<…>`-typed `CommandLineArgs`/`EnumNames`/`EnumValues` (v0.222) | per the landing revisions | various |
| Factory announcement | unmarked static bare-class returns | `owned`/Task-family/iterator announcement | v0.201 (flag-only) |

## Results

**38 sites fixed across 15 proposal files, plus 4 contract-clause sites in `future-directions-2026-07-18.md`; 19 files clean; 5 flags.**

| File (pin) | Fixed |
|---|---|
| 0002 (none) | 3 lambdas; 1 expression-`case` → paren atom |
| 0004 (v0.216) | 2 lambdas (refresh predated v0.217) |
| 0009 (v0.177) | 1 stale-API mention (`ReadOnlyAsciiByteSpan` dropped from the §D5 implementer list) |
| 0011 (v0.177) | 4 lambdas |
| 0013 (v0.177) | 1 statement `else if` → `elif` (caught by the verification pass; the nested form did not parse under A.6's single-`end` example shape) |
| 0014 (v0.177) | 3 lambdas |
| 0015 (v0.177) | 1 lambda |
| 0019 (v0.177) | 2 expression-`case`; 1 conditional atom |
| 0020 (v0.177) | 1 lambda |
| 0022 (v0.177) | 2 lambdas |
| 0026 (v0.177 base) | 4 lambdas (the 2026-07-28 addendum was already current) |
| 0029 (v0.177) | 1 stale-API mention (same class as 0009) |
| 0030 (v0.177) | 1 conditional atom (live recommended idiom, not an exhibit) |
| 0031 (v0.177) | 2 expression-`case`; 1 expression-`if` (nested `else if` → `elif`) |
| 0053 (v0.197) | 2 lambdas; 4 contract clauses; 1 postfix conditional (now matches spec §22.15's `Checked` example verbatim) |

Clean (audited, no edit): 0001, 0006, 0007, 0008, 0010, 0016, 0017, 0018, 0021*, 0024, 0025, 0027, 0028, 0040, 0047, 0052, 0054, 0060, 0061, and the light-pass set 0068–0074 + the three exploration/comparison records. (*See flag 3.)

## Flags — need a decision or a dedicated refresh, not a respelling

1. **0005 (self-hosting collections)** — the whole document predates the v0.168 `Owning*` renames (`OwnedStack`, `IOwnedCollection`) and carries stale `src/` anchors; its own provenance banner already mandates a mechanical refresh before any run acts on it. Additionally its `OwnedStack<T>: IOwnedCollection<T>` conformance claim is invalidated by the v0.222 demotion (#1811) — a design-shape change, not a rename. Left as-is: a partial rename would strip the visible staleness signal while the substance stayed wrong.
2. **0012 (`TimeZoneInfo`)** — `FindSystemTimeZoneById(id): TimeZoneInfo` (and its Try twin) is an unmarked static bare-class return, a declaration-site error under the v0.201 factory-announcement rule; the proposal *deliberately* designs borrow-of-interned semantics, and v0.223 (#1819) declined re-admitting static methods as borrow re-exposers. The marker choice (owned return / property-shaped lookup / a new admission) is an owner-level design question for 0012's graduation.
3. **0021 (structured logging) §4.3** — target-typed `new(…)` in a constructor-argument position, outside the shorthand's admitted contexts. Not a retired form; a drafting slip to fix at 0021's next design pass.
4. **0031 (Verify/Debug) §6.1** — catch arms spelled `catch ex: T` with statement-block bodies — no `var`, no `=>`; the current form is `catch var ex: T => do … end` (A.6 CatchArm). Catch syntax was outside this sweep's inventory; fix at 0031's next pass.
5. **0031 — `Verify.Unreachable(message): VerificationException`** — a static bare-class-returning factory; whether exception factories are exempt from the v0.201 announcement rule is a genuine design question (the intended idiom is `throw Verify.Unreachable(…)`, where the exception is immediately consumed). Flagged for 0031's graduation review.

**Provenance.** Two independent sweep passes (0001–0018; 0019–0074 + records), each self-verifying its diff hunk-by-hunk as pure respellings, followed by an independent adversarial verification pass over the combined diff before landing. The verification pass re-derived every hunk against Appendix A (all 37 confirmed pure respellings), caught the 0013 miss and this record's own count/citation errors, ran the residual-form greps clean across the live set, and confirmed the superseded proposals' retired spellings are the deliberate historical exhibits the sweep scoped out.
