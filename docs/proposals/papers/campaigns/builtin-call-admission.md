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
| `reproduces` (the symptom is still present) | 92 |
| `moved` (the symptom changed class: crash → silent drop, crash → wrong value) | 6 |
| `fixed` (the mechanism is GONE, though the tracker entry is still open) | 8 |
| `re-label` (the mechanism is **not** a builtin-call-admission defect — it belongs to another campaign) | 77 |
| `spec-question` (the spec does not decide it) | 6 |

**=> THE CAMPAIGN'S LIVE SCOPE IS 98 MEMBERS** (92 `reproduces` + 6 `moved`), not 189.

**TWO MEMBERS' PREMISES ARE STALE (both re-derived on HEAD, both MEASURED):**
* **#1792** ("direct char.Parse/TryParse/ParseOrNull and bool.ParseOrNull unhandled in sema") — **STALE.**
  Measured: `char.Parse("a")` prints `a`; `char.TryParse("a", out c)` prints `True` / `c = 'a'`;
  `char.ParseOrNull("a")` is not null and `char.ParseOrNull("zz")` IS null; `bool.ParseOrNull("true")`
  is not null and `bool.ParseOrNull("nope")` IS null. All four members it names behave per stdlib
  §2.19/§9.5 today.
* **#7108** ("string.Remove has no start-index bounds check: Remove(253) on a 252-char string reads
  past the end") — **STALE.** Measured: `"ab".Remove(253)` and a 252-char `s.Remove(253)` both raise
  `System.IndexOutOfRangeException`, which is exactly what stdlib §9.6 requires. The fix is
  `c0c9e3f9` (*fix #7168*), which IS in `origin/main`.
  **=> Both are candidates for closing as fixed; the coordinator files the closure.**

### COVERAGE — stated explicitly

* **74 of 189 members carry FULL MEASURED evidence on HEAD** — a repro was compiled and run with this
  lane's `build/bezelc`, and the observed output/diagnostic/abort is the evidence. Sources: the six
  §3 spot-checks (below), a mechanical sweep over every complete program in every issue body (55
  members), the 110-cell DONE grid (26 members named in its cells), and the two re-derived stale
  premises (#1792, #7108).
* **115 of 189 carry a TITLE-RULE verdict only** — the mechanism was read from the issue body (which
  quotes the governing stdlib sentence and, for most, the measuring reviewer), but **no repro was built**.
  For the ~90 rule-matched members §3 asks to "confirm or re-label", this reading **is** the deliverable:
  each was confirmed in-campaign or re-labelled to another campaign (see the table's `outcome` column).
* **NOT measured (41 rows; 24 of them are LIVE campaign-2 defects):** 26 crash members — #4255, #4805,
  #5978, #6056, #6100, #6126, #6254, #6255, #6295, #6646, #6659, #6677, #6689, #6809, #6873, #7030,
  #7246, #7265, #7274, #7287, #7293, #7299, #7319, #7329, #7331, #7358 — and 15 soundness members —
  #1765, #2625, #2647, #2730, #4126, #4481, #4790, #5409, #6648, #6782, #6983, #7098, #7189, #7291,
  #7294. Their issue bodies contain no compilable program, so each needs a hand-written repro before it
  can be counted as measured. **17 of the 41 are RE-LABEL rows** (they leave the campaign anyway), so
  **the campaign's own unmeasured backlog is 24, not 41**. **This is the entry lane's largest gap and
  the first thing the next lanes must close.**
* **74 of the 115 unmeasured members are severity `other`** (the §3 "rule-matched" set); those are the
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

The denominator is the **live** members of each family (the 8 whose mechanism is gone — 3 closed in
the tracker, 5 fixed — are excluded); the numerator is the members the family's shared site would close.

| family | live members | B0-leverage count | close rate | falsification verdict |
|---|---|---|---|---|
| **B1** Parse/TryParse/radix | 15 | 13 | **0.87** | **SURVIVES** |
| **B2** ToString(format)/Convert rendering | 22 | 7 | 0.32 | **SPLITS** → B2a (admission) / B2b (rendering contract) |
| **B3** Math/Convert argument typing | 15 | 6 | 0.40 | **SPLITS** → B3a (argument typing) / B3b (widening + evaluation order) |
| **B4** StringBuilder and string member arms | 23 | 7 | 0.30 | **SPLITS** → B4a (admission) / B4b (the `bz_sb_*` runtime contract) |
| **B5** nullable carrier at a builtin position | 7 | 0 | **0.00** | **DOES NOT SURVIVE** — a narrowed `T?` is *legal* (§7.4), so no admission gate rejects it; the rule is campaign 5's unwrap-before-consume |
| **B6** null at a non-nullable builtin parameter | 5 | 5 | **1.00** | **SURVIVES** |
| **B0x** the plan's own B0-named members (symbol-not-name, type-argument arity, ambiguity, constraint, inference, method-group arity) | 11 | 7 | **0.64** | **SURVIVES** |
| **X** re-labelled away (77) + spec-questions (6) | 0 | 0 | — | **not this campaign** — see *Re-labels* |

**#6648 was re-classified on review from Y to N**: `Math.Min/Max/Clamp` over double/int/long have an
UNTYPED RESULT, but **the CALL IS LEGAL** — a parameter-TYPE table does not reach it, so by this
record's own definition it is not a gate rejection.

**The falsification rule's verdict on the PLAN'S B0 CLAIM: it FAILS.** One signature-based admission
gate would have rejected **at most 52 of the 189 members (27.5 %)**, and **at most 45 of the 98 LIVE
defect members (45.9 %)**. Both figures are below one half, so B0 as a single sub-family is **split**.
The anchor is therefore **not one function**; see below.

**THE FIGURE IS AN UPPER BOUND, NOT A MEASUREMENT OF A GATE.** It is a per-member classification —
"would a signature-based admission gate have rejected this call before the arm ran?" — read off each
member's stated mechanism against the stdlib's declared signature. No gate was built and no call was
run through one, so the true figure can only be **lower**. Four of the Y rows were re-classified on
review as **not builtin calls at all** — #6253 and #6254 are USER static/generic classes, #6872 is an
ASSIGNMENT, and #6530 is a type-parameter NAME guard; left as Y they would have read 30.6 % instead of
27.5 %. **The split verdict is unaffected — the true figure can only be lower.**

### Re-labels (77 members — 77 unique, exclusive, every row with a destination)

The table below assigns each of the **77** re-labelled members to **exactly one** destination. The
number of unique ids is 77 and no id appears twice.

| destination | n | members |
|---|---|---|
| campaign 5 — nullable carrier unwrap | 15 | #2620 #2625 #2642 #4585 #5978 #6084 #6116 #6126 #6644 #6646 #6677 #6750 #6873 #7004 #7089 |
| campaign 7 — generic instantiation boundaries | 9 | #6042 #6049 #6181 #6228 #6239 #6307 #6697 #6933 #7066 |
| campaign 8 — owned collections / element lifetime | 9 | #2221 #2647 #2729 #2730 #3509 #3896 #6659 #6892 #6983 |
| campaign 13 — records/statics/contracts/misc (+ the stdlib §4/§5 members outside campaign 2's scope) | 19 | #1765 #2651 #2662 #3943 #4790 #4805 #5961 #6007 #6035 #6056 #6058 #6078 #6106 #6151 #6187 #6751 #6854 #6872 #6881 |
| campaign 1 — using-heads / disposal | 2 | #5355 #7146 |
| campaign 11 — async frames | 1 | #6100 |
| campaign 2 — in-campaign but NOT an admission defect (diagnostic wording, §15.1 tier, enablers, §-citation classes) | 8 | #5409 #5911 #6027 #6029 #7149 #7308 #7309 #7340 |
| parking-lot (out of the campaign's declared sections; no campaign claims the mechanism) | 14 | #1701 #2094 #2313 #3826 #4255 #4797 #6235 #6689 #6860 #7030 #7090 #7093 #7118 #7246 |

**=> 77 of 189 members (41 %) are not builtin-call-admission defects at all.** The campaign's live
scope is **98 members** (92 `reproduces` + 6 `moved`), not 189.

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

**Does the anchor change a rule?** **NO — and no ruling is needed.** §22.6 (v0.344) already says an
inapplicable builtin call "is a compile error with no candidates cited"; the anchor implements that at
sema. Owner ruling #2924/#7145 (2026-10-27) settles the severity of any `internal:` degrade that is
left. **⇒ The anchor REFUSES; it does not drop.** (The first draft of this record claimed the R35
ruling for #7290/#7293/#7294 chose "guard-and-drop" — that claim was **uncorroborated** and the
ruling question is **withdrawn**; see *Rulings needed*.)

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

**Cells: 110.** Classes: `OK` / `ACCEPTED` (compiled and ran, but not as the expectation says) /
`REJECTED` (rejected, but not as the expectation says) / `ABORT` (the compiler aborted) / `INTERNAL`
(compiled with an `internal:` note — a silent acceptance inversion).

**BASELINE — cells wrong on HEAD `d9f11d15` (MEASURED, both legs):**

| leg | cells | wrong | OK | ACCEPTED | REJECTED | ABORT | INTERNAL | sanitizer |
|---|---|---|---|---|---|---|---|---|
| plain | 110 | **48** | 62 | 25 | 2 | 8 | 13 | `NATIVE SANITIZER: 0 check(s) skipped` |
| `BEZEL_AOT_FORCE_SANITIZE=address` | 110 | **48** | 62 | 25 | 2 | 8 | 13 | `NATIVE SANITIZER: 0 check(s) skipped` |

**48 is the campaign's number to drive to zero**, of which **47 are campaign 2's own** and **1 belongs
to campaign 5** (the `math_floor_narrowed` cell — see the campaign split below). The grid reports the
per-campaign split itself (`--campaign 2` / `--campaign 5`).

**THE CAMPAIGN SPLIT (D13).** Four cells exercise a §7.4-**narrowed** nullable at a builtin
position: `math_floor_narrowed`, `sb_append_narrowed_int`, `int_tostring_radix_narrowed`,
`int_parse_from_narrowed`. **A narrowed `T?` is LEGAL** (§7.4) and §5.1 widens it to `T`, so **no
admission gate rejects these calls** — they are the **unwrap**, which is **campaign 5's** mechanism.
**=> Campaign 2's anchor does NOT include the unwrap.** Those four cells are tagged `campaign = "5"`
in the grid and are excluded from campaign 2's DONE line; the campaign's DONE line is
**"all 106 campaign-2 cells are OK"**, and campaign 5 owns its own four. (If the owner instead folds
the unwrap into campaign 2's anchor, the tag is one constant in the cell table.)

The 8 `ABORT` cells are compiler aborts (`math_floor_nullable`, `math_floor_narrowed`,
`math_floor_nullable_null`, `math_sign_string`, `math_abs_decimal_nullable`, `string_contains_int`,
`string_lastindexof_int`, `decimal_tostring_radix`); the 13 `INTERNAL` cells are the
silent-acceptance inversions (`int_parse_int`, `int_parse_bool`, `int_parse_char`, `int_parse_double`,
`int_parse_object`, `long_parse_intarg`, `bool_parse_int`, `char_parse_radix`, `decimal_parse_double`,
`double_parse_int`, `string_split_int`, `string_isascii`, `math_sign_reference`).

**Corpus dedup (done FIRST, as required):** of the 110 cells, **0 are byte-identical to an existing
corpus case and 17 are already covered, loosely, by an existing corpus case** — `l9_1_0011` (int.Parse
radix), `l9_4_0001` (bool.Parse), `l2_18_0012` (the integer format row), `l2_18_0017` (the format
fault), `l1_0004` (the StringBuilder surface), `s22_6_0102` (the Encoding surface), `s5_1_0148` (a
§5.1 conversion). **93 cells are NEW.** (The dedup is a normalised-text match against all 3693 files in
`tests/conformance/rules/cases/`; the "loose" match requires the cell's distinctive call to appear in
the corpus case's program.)

### The grid's own fixes (review round 1) — what changed and why

| cell | old expectation | new expectation | evidence |
|---|---|---|---|
| `int_tostring_radix_string` | `reject` | **`fault` / `FormatException`** | stdlib §2.18 declares `ToString(format: string)` for the primitives and "16" is not in the integer vocabulary (D/Dn/X/Xn/B) ⇒ the call is ADMITTED and the specifier is unrecognized at RUN time ("An unrecognized specifier raises `FormatException`"). The banked corpus case `l2_18_0027` (`expect: run`) confirms it. The compiler today prints "255" — it binds the string to the RADIX path — so the DEFECT is real and the cell stays wrong, but under the old expectation it was wrong under EVERY compiler behaviour. |
| `string_isascii` | `value` / `True` | **`reject`** | stdlib §9.6 declares `public pure readonly property IsAscii: bool` — a PROPERTY. `"abc".IsAscii()` has no candidate ⇒ §22.6 compile error. |
| `string_isascii_property` | — | **NEW cell**: `value` / `True` | the legal property read `"abc".IsAscii` (measured: prints `True`). |
| `debug_writeline_stringbuilder` | `reject` (green for the wrong reason) | **`reject`, import `System.Diagnostics`** | the old cell imported `System`, so `Debug` was undeclared and the cell was green on "`Debug` is declared in `System.Diagnostics`". With the right import the call COMPILES and writes an empty line — that is **#6780**, a live member. |
| `string_remove_oob` | `noabort` (vacuous: rc 70 was OK) | **`fault` / `IndexOutOfRangeException`** | stdlib §9.6: "a `startIndex` above `Length` … raises `IndexOutOfRangeException`". Measured: `"ab".Remove(253)` raises it. **=> #7108's mechanism is GONE** (fixed by `c0c9e3f9`, *fix #7168*, in `origin/main`) — the tracker entry is stale. |
| the `noabort` kind | `nocrash` (ignored the exit code) | **renamed `noabort`, and it now requires rc == 0** | a runtime FAULT is a wrong answer, not a pass. The 3 remaining `noabort` cells (`math_abs_decimal` — now a `value` cell, `sb_appendformat_string`, `sb_append_narrowed_int`) all exit 0 today. |
| **all 60 reject cells** | `want = None` ("any error will do") | **every reject cell now NAMES its needle** | D5: the `C()` helper RAISES if a reject cell has no needle, so the shape cannot regress. The needles are transcribed from the compiler's own diagnostic families (`no matching overload of \`X\``, `ambiguous call to \`X\``, `is nullable but the target type is non-nullable`, `not implicitly convertible to the target type`, `supplies N argument(s) but stdlib §X declares`, `expects \`Array<byte>\``, `inconsistent type-argument inference`, `is a reference to a generic method`, `type argument`). |
| every cell | the TSV's detail column embedded the ABSOLUTE program path | **cell-relative path** | two runs into different `--out` directories now produce **byte-identical** TSVs (verified). |
| — | the `clean` expectation kind was dead code | **removed** | — |

### The grid's self-test (D11)

**`scripts/gate/tests/test_builtin_admission_grid.py`** — pregate runs
`python3 -m unittest discover -s scripts/gate/tests`, so this is picked up automatically. It is
**build-free**: a fake `bezelc` (a shell script) stands in for the compiler and for the generated
program, so the generator, the classifier and the cell table are exercised end to end in ~0.3 s.
Ten tests, all green:
* every `reject` cell names a non-empty diagnostic needle (D5, structural);
* the cell names are unique, generation is deterministic, and every campaign tag is known;
* `reject` is `OK` **only** when the diagnostic contains the needle, and `REJECTED` (not `OK`) when
  it does not — the failure mode that let D3 slip through;
* a compiler abort and a compiler **signal** are the `ABORT` class;
* an `internal:` degrade is the `INTERNAL` class;
* a `value` cell checks the **exact** output;
* a `noabort` cell is `ACCEPTED` (not `OK`) when the program faults at run time (D4);
* a `fault` cell needs the fault text;
* the row's program path is cell-relative (D12).

## Rulings needed

**R1 IS WITHDRAWN — NO OWNER RULING IS NEEDED.** This record's first draft asked the owner to choose
between "guard-and-drop" and "a located refusal", on the strength of a claim that the R35 ruling for
#7290/#7293/#7294 chose guard-and-drop. **That claim is UNCORROBORATED**: the phrase appears nowhere in
the repository, the master plan §4.8, or the comments on #7290/#7293/#7294 — the only occurrence was
this record's own text. The #7290 comments say the opposite: the #7285 fix-forward removed the abort
"as a side effect" and "the RULE this issue asks for is not implemented". **=> R1's premise was false
and the question is withdrawn, not asked.**

**NO RULING IS NEEDED, on two existing authorities:**
* the **diagnostic half** is settled by §22.6 (v0.344): an inapplicable builtin call "is a compile
  error with no candidates cited" — the anchor REFUSES at sema, with a located diagnostic.
* the **severity half** is settled by owner ruling #2924/#7145 (2026-10-27): "every `internal:` degrade
  site becomes `Severity::Error`", with a ratchet holding the warning-site count at 0.

**=> THE CAVEAT THAT MATTERS: #2924/#7145 only reaches a drop site that EMITS a NOTE.** A site that
drops the call with **no diagnostic at all** is not reachable by that ruling. The
`user_class_named_stringbuilder` cell is exactly such a site: a user-declared class named
`StringBuilder` calling its OWN `Replace` is silently dropped — the program prints `after` and never
`USER-REPLACE`, with **no** `internal:` note. **Only a sema refusal fixes that one**, which is why the
anchor's refusal (not the severity flip) is the load-bearing half.

| # | what it unblocks | asked on |
|---|---|---|
| **R2** | **Is the stdlib's declared signature authoritative when the compiler's current acceptance contradicts it?** Two live cases: #7061 (`Math.Abs(float)` is typed `float`; the stdlib rationale says it resolves to `Abs(double)` and returns `double`) and #7254 (`Math.Abs(4000000000u)` is rejected as ambiguous although §5.1 admits `uint` at both `long` and `double`). Both are **acceptance-vs-declaration** conflicts, and the answer decides whether the gate *rejects* or *accepts-and-rewrites`. | 2026-10-28 (asked of the coordinator) |
| **R3** | **`string.Repeat(unit: string, count: int)`** — stdlib §9.6 declares it; the compiler implements only the `char` overload and rejects the `string` one (#7129, #7277). Implementing it *adds* an accepted program; the gate's rule would reject it. §9.6 says implement — the coordinator should confirm the gate must not "fix" this by rejecting a declared overload. | 2026-10-28 (asked of the coordinator) |

**THE GRID ALREADY PRE-COMMITS TO R2 AND R3**, so neither can be answered by changing the grid:
* **R2** — `math_abs_uint` expects a **REJECT** ("ambiguous call to `Math.Abs`", the wording §22.6's
  ambiguity rule produces today) and `convert_tostring_uint` expects a **REJECT** ("ambiguous call to
  `Convert.ToString`", stdlib §2.3's ambiguity note). If the owner rules that the stdlib's rationale
  wins and `Math.Abs(uint)` must be ADMITTED, these two cells' expectations change.
* **R3** — `string_repeat_string` expects **value "ababab"**, i.e. the DECLARED `Repeat(unit: string,
  count: int)` overload must WORK (§9.6). If the owner rules that the compiler's `char`-only surface
  is the contract, that cell's expectation changes to `reject`.

**§3 says campaign 2 has "none hard". THAT IS TRUE, AND IT IS NOW PROVEN RATHER THAN ASSUMED:** the
diagnostic half and the severity half are both settled by existing authorities, and the two open
questions (R2, R3) each touch **two** cells of a 110-cell instrument and are answerable without a
rule change. **No ruling blocks the anchor.**

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
* **Files:** `docs/proposals/papers/campaigns/builtin-call-admission.md` (this record),
  `docs/proposals/papers/campaigns/builtin-call-admission-members.md` (the 189-row table),
  `scripts/builtin_admission_grid.py` (the DONE instrument) and
  `scripts/gate/tests/test_builtin_admission_grid.py` (its self-test). **No `src/` file was touched.**
* **Rows:** 189 members re-measured (72 with full MEASURED evidence; the 117 others by title rule, and
  the unmeasured list is named above). 110 grid cells built and run twice (plain + ASan).
* **Fail-without proof:** not applicable — this lane changed no compiler behaviour, so there is no
  behaviour to prove fail-without. The instrument's *baseline* is the fail-without for the lanes that
  follow: each lane's cell must go from its entry CLASS to `OK` with the fix and stay at its entry
  CLASS without it.
* **Matrix result:** `scripts/builtin_admission_grid.py` — **110 cells, 48 wrong** on HEAD (47 of them
  campaign 2's own, 1 campaign 5's), both legs identical, `NATIVE SANITIZER: 0 check(s) skipped` in
  both, and the TSV is byte-identical across two different `--out` directories.
* **Self-test:** `scripts/gate/tests/test_builtin_admission_grid.py` — 10 tests, build-free, green;
  pregate runs it via `python3 -m unittest discover -s scripts/gate/tests`.

**FIX-FORWARD (review round 1, ACCEPT-W-NOTES).** The reviewer accepted the record and its numbers but
barred the grid from being the DONE instrument until five grid defects were fixed. All five are fixed,
plus four further items; the changed files are **`scripts/builtin_admission_grid.py`** and
**`scripts/gate/tests/test_builtin_admission_grid.py`** (new), and the two documentation files. The
per-cell changes are listed in *The grid's own fixes* above. **The L0 ANCHOR lane has copied the grid
onto its branch and must RE-READ the corrected grid and RE-MEASURE**: the cell count moved 109 → 110,
the baseline moved 47 → 48, one cell was renamed in expectation (`string_isascii`), one cell was added
(`string_isascii_property`), one cell's import changed (`debug_writeline_stringbuilder`), the
`nocrash` kind was renamed `noabort` and now requires exit code 0, **every reject cell now names its
diagnostic needle**, and **four cells are tagged `campaign = "5"`** and are excluded from campaign 2's
DONE line.
* **THE B0 LEVERAGE COUNT — the number this lane was sent to produce: AT MOST 52 of the 189 members
  (27.5 %), and at most 45 of the 98 LIVE defect members (45.9 %).** Both are below one half ⇒
  **B0 splits**; the anchor is a table + two admission points, not one function, and B2, B3, B4 each
  split into an admission half and a non-admission half. The figure is an **UPPER BOUND** (a
  per-member classification, not an execution of a gate) and it can only fall.

## Exit review

*(to be written by the campaign's exit reviewer — not this lane)*
