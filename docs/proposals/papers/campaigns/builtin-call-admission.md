# Campaign builtin-call-admission — Built-in call admission: typed arms for the stdlib surface

**Track:** sema/stdlib (the other track; disjoint from campaign 1's `LlvmBackend_Disposal.cpp` /
`registerUsingDisposal` / the sema vacuity check).
**Sections:** spec §22.6 (applicability and ambiguity), §22.5 (named arguments), §5.1 (conversions),
§7.3/§7.4 (nullability at parameters), §17.4; stdlib §2 (primitives: `Parse`/`TryParse`/`ToString(format)`/
radix, `Math`, `Convert`), §3 (`StringBuilder`, `Encoding`), §9.6 (string members). Spec **v0.345**.
**Shared code paths** (read at HEAD `d9f11d15`):

| path | file:function @ `d9f11d15` | what it decides today |
|---|---|---|
| S1 | `src/sema/ExprTypecheck_Calls.cpp` :: `ExprTC::typeCall` | the per-call driver; the seven `checkBuiltin*` calls sit in one contiguous block at its tail |
| S2 | `src/sema/ExprTypecheck_Calls.cpp` :: `ExprTC::checkBuiltinStringBuilderCall` (table `builtinStringBuilderSurface()`, 8 rows) | StringBuilder arity + per-position kind |
| S3 | `src/sema/ExprTypecheck_Calls.cpp` :: `ExprTC::checkBuiltinMathConvertCallArgs` (table `builtinMathConvertSurface()`, 34 rows) | Math/Convert argument types + §22.6 ranking |
| S4 | `src/sema/ExprTypecheck_Calls.cpp` :: `checkBuiltinStringCallArity` (tables `builtinStringInstanceArities()` 24 / `builtinStringStaticArities()` 4 / `builtinStringUndeclaredArities()` 3) | string arity only — **separate** from the string argument-type lambda in `checkBuiltinStringCallArgs` |
| S5 | `src/sema/ExprTypecheck_Calls.cpp` :: `ExprTC::checkBuiltinNullableArgs` (registry `common::builtinParamIsNonNullable`, 93 rows in `BuiltinRegistry.cpp`) | §7.3 nullability — **no `Math` rows at all** |
| S6 | `src/sema/ExprTypecheck_Calls.cpp` :: `ExprTC::checkBuiltinNamedArgs` + `builtinCallHeadHonouringArgNames` | §22.5 named-argument binding, for **six heads only** (`string`/`String`, `Encoding`, `StringBuilder`, `Math`, `Convert`) |
| S7 | `src/codegen/LlvmBackend_Calls.cpp` :: `Emitter::tryLowerCollectionInstanceCall` | `isStrBld = (recvTy && recvTy->headName() == "StringBuilder")` — **head name, no symbol check** (the #7217/#6845 site) |
| S8 | `src/codegen/LlvmBackend_Calls.cpp` :: `Emitter::tryLowerBuiltinClassCall` | the class-spelling `Parse`/`TryParse` arms, **all** the `Math` arms, and `Convert.ToString` |
| S9 | `src/codegen/LlvmBackend_Calls2.cpp` :: `Emitter::tryLowerPrimitiveStaticCall` | the keyword-spelling `Parse`/`TryParse` arms (a **second copy** of S8's arms, in a different file) |
| S10 | `src/codegen/LlvmBackend_Calls2.cpp` :: `sharedParseOperandAdmissible` (forward-declared in `LlvmBackend_Calls.cpp`) | the **one** shared first-argument guard — **shape-based, not signature-based** |
| S11 | `src/codegen/LlvmBackend_String.cpp` :: `Emitter::emitWithFormatSpec` / `emitPrimitiveToString` | the format-specifier path |
| S12 | `src/codegen/LlvmBackend_Calls.cpp` :: `Emitter::builtinTypeName` | the **one** symbol-aware receiver discriminator (the #2997 discriminator) |

**Anchor:** *not one function* — see *The anchor* below. It is **one table + two admission points**:
a parameter-**type** signature table in `src/common/BuiltinRegistry.cpp` (which already owns the
parameter-**name** rows for the same six heads and the 93-row nullability table, but **no**
parameter-type rows), consulted by **one** sema admission function
(`ExprTC::checkBuiltinCallAdmission`, to be added to the S1 block) and by **one** codegen receiver
discriminator (`Emitter::builtinTypeName`, S12, replacing the three head-name-only sites).

## Members at entry

189 members at entry (`docs/proposals/papers/master-plan/work/out/campaign-members.tsv`, rows where
`campaign == builtin-call-admission`). **186 are OPEN at HEAD**; three were closed on 2026-10-27 and are
recorded as `fixed-by`:

| # | closed by | in `origin/main`? |
|---|---|---|
| #7325 | `b1adfab95` (fix #7325 — the floating Parse arms reject a signed bare point) | yes |
| #7329 | `275fab52` (fix #7329 — the integer row's D and X specifiers work at every width) | yes |
| #7341 | `d9f8b36f` (fix #7341 — a point with no digits after it is rejected) | yes |

**Re-measured on HEAD `d9f11d15` (bezelc built in this lane's worktree).** Verdicts:

| status | count |
|---|---|
| `reproduces` (the symptom is still present) | 94 |
| `moved` (the symptom changed class: crash → silent drop, crash → wrong value) | 6 |
| `fixed-by` (the closing commit is in `origin/main`) | 6 |
| `re-label` (the mechanism is **not** a builtin-call-admission defect — it belongs to another campaign) | 77 |
| `spec-question` (the spec does not decide it) | 6 |

### COVERAGE — stated explicitly

* **72 of 189 members carry FULL MEASURED evidence on HEAD** — a repro was compiled and run with this
  lane's `build/bezelc`, and the observed output/diagnostic/abort is the evidence. Sources: the six
  §3 spot-checks (below), a mechanical sweep over every complete program in every issue body (55
  members), and the 109-cell DONE grid (26 members named in its cells).
* **117 of 189 carry a TITLE-RULE verdict only** — the mechanism was read from the issue body (which
  quotes the governing stdlib sentence and, for most, the measuring reviewer), but **no repro was built**.
  For the ~90 rule-matched members §3 asks to "confirm or re-label", this reading **is** the deliverable:
  each was confirmed in-campaign or re-labelled to another campaign (see the table's `outcome` column).
* **NOT measured (41):** 26 crash members — #4255, #4805, #5978, #6056, #6100, #6126, #6254, #6255,
  #6295, #6646, #6659, #6677, #6689, #6809, #6873, #7030, #7246, #7265, #7274, #7287, #7293, #7299, #7319,
  #7329, #7331, #7358 — and 15 soundness members — #1765, #2625, #2647, #2730, #4126, #4481, #4790, #5409,
  #6648, #6782, #6983, #7098, #7189, #7291, #7294. Their issue bodies contain no compilable program, so
  each needs a hand-written repro before it can be counted as measured. **This is the entry lane's
  largest gap and the first thing the next lanes must close.**
* **76 of the 117 unmeasured members are severity `other`** (the §3 "rule-matched" set); those are the
  ones §3 explicitly asks to *confirm or re-label*, and each has been re-labelled.

The full 189-row table (one row per member: severity, HEAD status, evidence, sub-family, lane,
B0 leverage) is in `docs/proposals/papers/campaigns/builtin-call-admission-members.md`, on this branch.

### The six mechanisms §3 quotes — MEASURED, not assumed

| # | §3's claim | MEASURED at HEAD `d9f11d15` | verdict |
|---|---|---|---|
| #7290 | a non-string first argument to `int.TryParse` aborts the compiler | **the abort is GONE.** `int.TryParse(16, out v)` now compiles, with `warning: internal: lowerLocalDecl's initializer expression failed to lower to a value for this local's slot (degraded to a zero-initialized value) — #2434`, and the program prints `False` / `0`. The control `int.TryParse("16", out v)` prints `16`. | **MOVED** — crash → *silent acceptance inversion* (the illegal call is accepted with no §22.6 diagnostic) |
| #7300 | the decimal arms are "a second copy in a different file" | **CONFIRMED in the code**: `LlvmBackend_Calls2.cpp::tryLowerPrimitiveStaticCall` holds the keyword spelling, `LlvmBackend_Calls.cpp::tryLowerBuiltinClassCall` holds the class spelling, and the second file *forward-declares* `sharedParseOperandAdmissible` from the first. **And the abort is GONE**: `decimal.TryParse(1.5, out m)` compiles with an `internal:` degrade and prints `False`/`0`; `decimal.Parse(1.5)` makes the statement emit nothing. Control `decimal.TryParse("1.5", out m)` prints `True` / `1.5`. | **MOVED** — crash → silent drop |
| #7217 / #6845 | a user class named `StringBuilder` is claimed by the builtin arm | **CONFIRMED in the code** (S7: `isStrBld = (recvTy && recvTy->headName() == "StringBuilder")`, no `declaration` check). **MEASURED**: a user `class StringBuilder` with its own `Insert(int, string)` → the generated program **SIGSEGVs** (rc -11); under ASan: `SEGV ... in bz_sb_content_cps ... bz_sb_replace` — the builtin runtime is run on the user's object. The `Replace(string, string)` shape no longer SEGVs but the user's own method is **silently dropped** (the program prints only `after`, never `USER-REPLACE`; the control with a differently-named class prints `USER-REPLACE`). | #6845 **REPRODUCES**; #7217 **MOVED** (SEGV → silent drop) |
| #7297 | nullable carriers reach `Math.Floor` unwrapped | **REPRODUCES.** `Math.Floor(d)` with `d: double?` → `Call parameter type does not match function signature!  %1 = load { i8, double }, ptr %0, align 8` → `bezelc: codegen error: IR verification failed` (rc 1). The **narrowed** shape (`if d <> null then Math.Floor(d)`) aborts identically. Control `Math.Floor(2.5)` prints `2`. | **REPRODUCES** (compiler abort) |
| #6864 | `Math.Max(3u, 4000000000u)` prints 3 | **FIXED.** It now prints `4000000000`. The control `a > b` prints `True` and `Math.Max(a, b)` prints `4000000000`. | **FIXED** (well-typed call — B0 never applies) |
| #7180 | a reordered named argument hands an `i1` to a `ptr` parameter | **MOVED.** The compiler abort is GONE. `"ABCDEF".Equals(ignoreCase := true, other := "abcdef")` now prints `True` (correct). But `"ABCDEF".CompareTo(ignoreCase := true, other := "abcdef")` is **REJECTED**: `the argument to `string.CompareTo` has a type that is not implicitly convertible to the target type (§5.1)` — a located diagnostic, not an abort. Control `CompareTo("abcdef", true)` prints `0`. | **MOVED** — crash → located rejection |

**Two of §3's six quoted mechanisms have already moved** (#7290, #7300, #7180, #7217), and one is
**fixed** (#6864). The plan's §3 mechanism paragraph is therefore a snapshot of an older tree; the
"quoted from the members" framing must not be read as current evidence.

## Sub-families and lane plan

§3 names B0–B6. **Verified against the code, and the September falsification rule was applied: a
sub-family whose shared site would close fewer than half its members is split.**

| family | members | B0-leverage count | close rate | falsification verdict |
|---|---|---|---|---|
| **B1** Parse/TryParse/radix | 17 | 13 | **0.76** | **SURVIVES** |
| **B2** ToString(format)/Convert rendering | 25 | 7 | 0.28 | **SPLITS** → B2a (admission, 9) / B2b (rendering contract, 16) |
| **B3** Math/Convert argument typing | 18 | 7 | 0.39 | **SPLITS** → B3a (argument typing, 8) / B3b (widening + evaluation order, 10) |
| **B4** StringBuilder and string member arms | 24 | 7 | 0.29 | **SPLITS** → B4a (admission, 7) / B4b (the `bz_sb_*` runtime contract, 17) |
| **B5** nullable carrier at a builtin position | 7 | 0 | **0.00** | **DOES NOT SURVIVE** — a narrowed `T?` is *legal* (§7.4), so no admission gate rejects it; the rule is campaign 5's unwrap-before-consume |
| **B6** null at a non-nullable builtin parameter | 5 | 5 | **1.00** | **SURVIVES** |
| **B0x** the plan's own B0-named members (symbol-not-name, type-argument arity, ambiguity, constraint, inference, method-group arity, diagnostics) | 18 | 10 | **0.56** | **SURVIVES** |
| **X** re-labelled away (75) + spec-questions (6) | 81 | 8 | 0.11 | **not this campaign** — see *Re-labels* |

**The falsification rule's verdict on the PLAN'S B0 CLAIM: it FAILS.** One signature-based admission
gate would have rejected **57 of the 186 live members — 30.6 %**. That is below one half, so B0 as a
single sub-family is **split**. The anchor is therefore **not one function**; see below.

### Re-labels (77 members confirmed out of campaign 2)

| goes to | members |
|---|---|
| campaign 5 (nullable carrier unwrap) | #2620, #2625, #2642, #4585, #6084, #6116, #6126, #6646, #6644, #6677, #6750, #6873, #7004, #7089, #5978, #6733 — 16 |
| campaign 7 (generic instantiation boundaries) | #6042, #6049, #6181, #6228, #6239, #6307, #6697, #6933, #7066 — 9 |
| campaign 8 (owned collections / element lifetime) | #2647, #2730, #3509, #3826, #3896, #2729, #6892, #6983, #6659 — 9 |
| campaign 13 (records/statics/contracts/misc) | #1765, #3943, #4790, #4805, #5961, #6007, #6027, #6029, #6035, #6056, #6058, #6078, #6106, #6151, #6187, #6751, #6872, #4797, #2313, #4255, #6860, #6689, #7030, #7090, #7093, #7118, #7246 — 27 |
| campaign 1 (using-heads / disposal) | #5355, #7146 — 2 |
| campaign 11 (async frames) | #6100 — 1 |
| stdlib §4 / §5 scope (outside the campaign's declared sections) | #2651, #2662, #3826 — 3 |
| parking-lot / spec-question (6) / docs | #1701, #2256, #4481, #4890, #5486, #5490, #5854 (spec-question); #2094, #2256, #2313, #4255, #7340 (parking-lot); #5409, #7149, #7308, #7309, #6881 (docs/citation) — 16 |

**=> 77 of 189 members (41 %) are not builtin-call-admission defects at all.** The campaign's live
scope is 112 members; the DONE instrument below is built for those.

### Lane plan (order, hot files, model tier)

| # | lane | owns | hot files | tier |
|---|---|---|---|---|
| **L0** | **the anchor**: the signature table + the two admission points | B1 (13 of 17) + B6 (5 of 5) + the table itself | `src/common/BuiltinRegistry.cpp`, `src/sema/ExprTypecheck_Calls.cpp` | **Opus**, alone |
| L1 | B0x — the receiver's SYMBOL, not its name | #6159, #6530, #6845, #7217, #7098, #5998, #6253, #6254, #6295, #7173, #6228, #4790, #5961, #6078, #6872 | `src/codegen/LlvmBackend_Calls.cpp` (`tryLowerCollectionInstanceCall`, `builtinTypeName`), `src/sema/ExprTypecheck_Calls.cpp` | Opus |
| L2 | B3a — Math/Convert argument typing | #7218, #7227, #7232, #6647, #6648, #7061, #7297 | `src/sema/ExprTypecheck_Calls.cpp` (`checkBuiltinMathConvertCallArgs`, `builtinMathConvertSurface`) | Sonnet |
| L3 | B2a — the format/Convert admission shapes | #7284, #7291, #7319, #7331, #7333, #7234, #7239, #6782, #6825 | `src/sema/ExprTypecheck_Calls.cpp`, `src/codegen/LlvmBackend_String.cpp` | Sonnet |
| L4 | B4a — StringBuilder/string admission shapes | #6809, #7180, #6635, #6636, #6826, #6780, #7173 | `src/sema/ExprTypecheck_Calls.cpp` | Sonnet |
| L5 | B2b/B3b/B4b — the *runtime* contracts (format text, radix text, `bz_sb_*` bounds) | #7258, #7265, #7287, #7293, #7299, #7311, #7336, #7358, #7359, #7361, #7362, #7303, #7210, #7221, #7228, #7229, #7244, #7245, #7154, #7167, #7189, #7108, #7249, #7252, #7253, #7219, #7292 | `src/codegen/LlvmBackend_String.cpp`, `src/runtime/…` | Sonnet — **NOT admission**; these are the campaign's *residual* and need their own rule |
| — | B5 — **not this campaign**; re-label to campaign 5 | 7 | — | — |

L0 must land before L1–L4 (the table is theirs). L5 is file-disjoint from L0–L4 and can run in
parallel from the start.

### The anchor

**It is three things, not one** — and that is the finding, not a defeat:

1. **One table.** `src/common/BuiltinRegistry.cpp` gains **parameter-TYPE rows** for the built-in
   surface: `(head, member, arity, position, paramKind)`, alongside the parameter-**name** rows it
   already owns for the same six heads (`kStringParamNames` 37, `kEncodingParamNames` 4,
   `kStringBuilderParamNames` 6, `kMathParamNames` 15, `kConvertParamNames` 12) and the 93-row
   nullability table (`kBuiltinNonNullableParams`). Today **no family except `StringBuilder` and
   `Math`/`Convert` has a parameter-type table at all**, and those two tables are independent of each
   other, of the nullability table, and of the name table.
2. **One sema admission point.** A new `ExprTC::checkBuiltinCallAdmission(const ast::CallExpr&)`
   called **once** from the S1 block in `ExprTC::typeCall`, which for a builtin head resolves the call
   against the table — arity, §5.1 conversions, §7.3 nullability, §22.5 named arguments, and the
   receiver's **symbol** — and emits **one** §22.6 diagnostic ("the call is a compile error with no
   candidates cited") when no candidate applies. The seven existing `checkBuiltin*` calls become the
   table's consumers rather than seven independent hand-written checks.
3. **One codegen receiver discriminator.** `Emitter::builtinTypeName` (S12) is the ONE place that
   answers "is this receiver the built-in, or a user type that merely spells the same name?". Three
   sites answer it by head name instead — `LlvmBackend_Calls.cpp::tryLowerCollectionInstanceCall`
   (`isStrBld`), `LlvmBackend_Setup.cpp::isPlusOneStringProducer`, `LlvmBackend_Exprs2.cpp::lowerNew` —
   and are the #7217/#6845 mechanism. Two more already use the symbol correctly
   (`LlvmBackend_Exprs.cpp::lowerExprInner`, `LlvmBackend_Setup.cpp::registerUsingDisposal`).

**Does the anchor change a rule?** *No new rule is needed for the rejection half* — §22.6 already
says the call is a compile error. *But* the R35 ruling for #7290/#7293/#7294 chose **"guard-and-drop"**
(a silent drop with an `internal:` degrade) over a located refusal, and **that contradicts §22.6**.
Owner ruling #2924/#7145 (2026-10-27) already answers it: *every `internal:` degrade site becomes
`Severity::Error`*. ⇒ **The campaign needs NO new hard ruling provided the guard-and-drop sites are
routed through that same path.** If the owner wants the drop to stay a warning, that IS a new ruling.

## DONE instrument

**What it is:** `scripts/builtin_admission_grid.py` (added by this lane) — a generator in the shape of
`scripts/regression_matrix.py` (deterministic generation, one program per cell, a compile+run leg per
cell, a per-cell CLASS, a TSV of every cell, a non-zero exit when a cell is wrong). Each cell is one
built-in call reached with a **receiver kind** × **argument kind** × **spelling**, and each cell states
what the compiler must do with it: accept with this **exact** output, reject with a located error,
raise this fault, or at minimum not abort the compiler.

**Where it lives:** `scripts/builtin_admission_grid.py` on this branch (`r36-c2-entry`). It is
gate-runnable: `BEZEL_AOT_FORCE_SANITIZE=address scripts/builtin_admission_grid.py --out DIR
--bezelc build/bezelc` compiles every cell with `--sanitize=address` and prints
`NATIVE SANITIZER: N check(s) skipped`.

**Cells: 109.** Classes: `OK` / `ACCEPTED` (compiled and ran, but not as the expectation says) /
`REJECTED` (rejected, but not as the expectation says) / `ABORT` (the compiler aborted) / `INTERNAL`
(compiled with an `internal:` note — a silent acceptance inversion).

**BASELINE — cells wrong on HEAD `d9f11d15` (MEASURED, both legs):**

| leg | cells | wrong | OK | ACCEPTED | REJECTED | ABORT | INTERNAL | sanitizer |
|---|---|---|---|---|---|---|---|---|
| plain | 109 | **47** | 62 | 24 | 2 | 8 | 13 | `NATIVE SANITIZER: 0 check(s) skipped` |
| `BEZEL_AOT_FORCE_SANITIZE=address` | 109 | **47** | 62 | 24 | 2 | 8 | 13 | `NATIVE SANITIZER: 0 check(s) skipped` |

**47 is the campaign's number to drive to zero.** The 8 `ABORT` cells are compiler aborts
(`Math.Floor(double?)`, `Math.Floor(double? narrowed)`, `Math.Floor(null)`, `Math.Sign("x")`,
`Math.Abs(decimal?)`, `string.Contains(1)`, `string.LastIndexOf(1)`, `decimal.ToString(16)`); the 13
`INTERNAL` cells are the silent-acceptance inversions (`int.Parse(16)`, `int.Parse(true)`,
`int.Parse('a')`, `int.Parse(1.5)`, `int.Parse(object)`, `long.Parse(16)`, `bool.Parse(1)`,
`char.Parse("a",16)`, `decimal.Parse(1.5)`, `double.Parse(1)`, `string.Split(1)`, `string.IsAscii()`,
`var g := Math.Sign;`).

**Corpus dedup (done FIRST, as required):** of the 109 cells, **0 are byte-identical to an existing
corpus case and 17 are already covered, loosely, by an existing corpus case** — `l9_1_0011` (int.Parse
radix), `l9_4_0001` (bool.Parse), `l2_18_0012` (the integer format row), `l2_18_0017` (the format
fault), `l1_0004` (the StringBuilder surface), `s22_6_0102` (the Encoding surface), `s5_1_0148` (a
§5.1 conversion). **92 cells are NEW.** (The dedup is a normalised-text match against all 3693 files in
`tests/conformance/rules/cases/`; the "loose" match requires the cell's distinctive call to appear in
the corpus case's program.)

## Rulings needed

| # | what it unblocks | asked on |
|---|---|---|
| **R1** | **The drop-vs-refuse conflict.** §22.6 (v0.344) says an inapplicable builtin call "is a compile error with no candidates cited"; the R35 ruling for #7290/#7293/#7294 chose **guard-and-drop** (an `internal:` degrade), which is what the campaign's 13 `INTERNAL` grid cells measure today. **The existing owner ruling #2924/#7145 (2026-10-27) — "every `internal:` degrade site becomes `Severity::Error`" — answers it**, so no NEW ruling is needed *if* the guard-and-drop sites go through that path. **If the owner wants the drop to remain a warning, a new ruling is required** and the campaign's DONE instrument changes shape. | 2026-10-28 (asked of the coordinator) |
| **R2** | **Is the stdlib's declared signature authoritative when the compiler's current acceptance contradicts it?** Two live cases: #7061 (`Math.Abs(float)` is typed `float`; the stdlib rationale says it resolves to `Abs(double)` and returns `double`) and #7254 (`Math.Abs(4000000000u)` is rejected as ambiguous although §5.1 admits `uint` at both `long` and `double`). Both are **acceptance-vs-declaration** conflicts, and the answer decides whether the gate *rejects* or *accepts-and-rewrites*. | 2026-10-28 (asked of the coordinator) |
| **R3** | **`string.Repeat(unit: string, count: int)`** — stdlib §9.6 declares it; the compiler implements only the `char` overload and rejects the `string` one (#7129, #7277). Implementing it *adds* an accepted program; the gate's rule would reject it. §9.6 says implement — the coordinator should confirm the gate must not "fix" this by rejecting a declared overload. | 2026-10-28 (asked of the coordinator) |

**§3 says campaign 2 has "none hard". THAT IS NEARLY TRUE, AND THE NEAR-TRUTH MATTERS:** R1 is
answered by an existing owner ruling, R2 and R3 are single-member questions, and the §2.18 / §9.1
format-and-radix *text* rules are all decided by the stdlib. **No ruling blocks the anchor.**

## Questions for the next train (filed spec-question numbers)

Six members are spec-questions and are **held out** of the lanes:
* **#4890** — `double`/`float` no-format `ToString()` renders NaN/Infinity as `"nan"`/`"inf"`; the
  internal diagnostic code uses `"NaN"`/`"Infinity"`. The spec is silent on which is required.
* **#5490** — stdlib §2.3 states no relation between `Convert.ToString(double)` and `double.ToString()`.
* **#5854** — is `override Equals(Object?)` on an enum/choice admitted or rejected? §10.5 vs §9.2
  disagree; the corpus banks the row `fail` and the compiler accepts.
* **#2256** — §22.15's closed pure-construction list omits choice construction.
* **#4481** — `char` classification and case conversion are ASCII-only above the #4420 guard.
* **#7308** — the `TryParse` out-slot default is attributed to stdlib §9.3 in **seven** places in
  `src/` and in `tests/`; §9.3 is the *decimal* section and says nothing about the out slot. This is a
  **§-citation class**, gate-enforceable by `scripts/check_src_citations.py`'s siblings, and it is the
  one member of the campaign that is a *documentation* defect.

## Siblings filed during the campaign

None. This lane filed no issue (lanes may not file); every defect found is reported to the coordinator
in the hand-back with a minimal repro and a §-citation (see *Lane records*).

## Lane records

**Lane `r36-c2-entry` (Opus, the ENTRY lane — this commit).**

* **SHAs:** see the hand-back. Branch `r36-c2-entry`, based on `origin/main` `d9f11d15` (fetched;
  `origin/main` had not moved).
* **Files:** `docs/proposals/papers/campaigns/builtin-call-admission.md` (this record) and
  `scripts/builtin_admission_grid.py` (the DONE instrument's skeleton). **No `src/` file was touched.**
* **Rows:** 189 members re-measured (72 with full MEASURED evidence; the 117 others by title rule, and
  the unmeasured list is named above). 109 grid cells built and run twice (plain + ASan).
* **Fail-without proof:** not applicable — this lane changed no compiler behaviour, so there is no
  behaviour to prove fail-without. The instrument's *baseline* is the fail-without for the lanes that
  follow: each lane's cell must go from its entry CLASS to `OK` with the fix and stay at its entry
  CLASS without it.
* **Matrix result:** `scripts/builtin_admission_grid.py` — 109 cells, **47 wrong** on HEAD, both legs
  identical, `NATIVE SANITIZER: 0 check(s) skipped` in both.
* **THE B0 LEVERAGE COUNT — the number this lane was sent to produce: 57 of 186 live members (30.6 %).**
  Below one half ⇒ **B0 splits**; the anchor is a table + two admission points, not one function, and
  B2, B3, B4 each split into an admission half and a non-admission half.

## Exit review

*(to be written by the campaign's exit reviewer — not this lane)*
