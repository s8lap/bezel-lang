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
`campaign == builtin-call-admission`). **184 are OPEN at HEAD**; **five** were closed on 2026-10-27:

| # | closed | in `origin/main`? |
|---|---|---|
| #7325 | 2026-10-27T11:49 by `b1adfab95` (fix #7325 — the floating Parse arms reject a signed bare point) | yes |
| #7329 | 2026-10-27T11:49 by `275fab52` (fix #7329 — the integer row's D and X specifiers work at every width) | yes |
| #7341 | 2026-10-27T11:49 by `d9f8b36f` (fix #7341 — a point with no digits after it is rejected) | yes |
| #1792 | **2026-10-27T13:43 by the coordinator (s8lap)** — the premise was stale (measured below) | — |
| #7108 | **2026-10-27T13:43 by the coordinator (s8lap)** — the mechanism is fixed by `c0c9e3f9` (*fix #7168*) | yes |

**Re-measured on HEAD `d9f11d15` (bezelc built in this lane's worktree), and RE-DERIVED FROM THE DONE
GRID'S OWN VERDICTS (2026-10-28, fix-forward 3).** Verdicts:

| status | count |
|---|---|
| `reproduces` (the symptom is still present) | 84 |
| `moved` (the symptom changed class: crash → silent drop, crash → wrong value) | 6 |
| `fixed` (the mechanism is GONE, though the tracker entry is still open) | 15 |
| `re-label` (the mechanism is **not** a builtin-call-admission defect — it belongs to another campaign, or the report's premise is wrong) | 78 |
| `spec-question` (the spec does not decide it) | 6 |

**=> THE CAMPAIGN'S LIVE SCOPE IS 90 MEMBERS** (84 `reproduces` + 6 `moved`), not 189. **ONE MEMBER LEFT THE LIVE SCOPE UNDER THE COORDINATOR'S R2 RULING (2026-10-28): #7254 (`Math.Abs(4000000000u)`) is NOT A DEFECT — the call is ambiguous under §22.6 and the compiler's rejection is correct — so it is re-labelled, and its grid cell `math_abs_uint` now pins that correct rejection.** (The `lane`
column says which lane owns the FIX; 11 of those 90 are owned by the `re-label` lane — their symptom
still reproduces, but the fix is a re-label, not an admission fix.)

**SEVEN MORE MEMBERS LEFT THE LIVE SCOPE WHEN THEIR STATUS WAS RE-DERIVED FROM THE GRID (fix-forward
3, 2026-10-28) — each measured on HEAD `d9f11d15`, and each written into the members table:**
* **#6648** — all three issue shapes are now REJECTED (§5.1's initializer diagnostic). The grid's
  `math_minmax_typed_result` cell does not reach the shape, so the status came from the member's own
  programs, not from the cell.
* **#6782** — `TimeSpan.ToString(null)` and `DateTime.ToString(null)` are REJECTED (§7).
* **#6809** — all four issue shapes are REJECTED with located diagnostics, and no malformed IR.
* **#7234** — `Convert.ToString('A')` and `Convert.ToString(255U)` are REJECTED as ambiguous (§2.3).
* **#7286** — all four named shapes are correct (the nullable radix is rejected; the foldable constant,
  the string-VARIABLE format and the run-time int radix all render the radix form).
* **#7296** — the NAMED-argument spelling of a primitive `ToString` radix is now nullability-checked
  like the positional one.
* **#7271** — `(255).ToString(16u)` renders the RADIX form "ff", not the decimal text. **WHAT IS LEFT
  OF #7271 IS A SPEC DISAGREEMENT, NOT AN ADMISSION DEFECT: the tracker asks for a compile error, and
  §5.1 (v0.345)'s value-based constant narrowing does not require one** (see *R4* below). It is
  recorded as `fixed` because the mechanism the tracker described — the decimal fallback — is gone;
  re-labelling it instead is the coordinator's call.

**TWO MEMBERS' PREMISES ARE STALE — AND BOTH ARE NOW CLOSED IN THE TRACKER (2026-10-27T13:43, by the
coordinator `s8lap`); this lane re-derived them and did not file anything:**
* **#1792** ("direct char.Parse/TryParse/ParseOrNull and bool.ParseOrNull unhandled in sema") — **STALE.**
  Measured: `char.Parse("a")` prints `a`; `char.TryParse("a", out c)` prints `True` / `c = 'a'`;
  `char.ParseOrNull("a")` is not null and `char.ParseOrNull("zz")` IS null; `bool.ParseOrNull("true")`
  is not null and `bool.ParseOrNull("nope")` IS null. All four members it names behave per stdlib
  §2.19/§9.5 today. **CLOSED 2026-10-27.**
* **#7108** ("string.Remove has no start-index bounds check: Remove(253) on a 252-char string reads
  past the end") — **STALE.** Measured: `"ab".Remove(253)` and a 252-char `s.Remove(253)` both raise
  `System.IndexOutOfRangeException`, which is exactly what stdlib §9.6 requires. The fix is
  `c0c9e3f9` (*fix #7168*), which IS in `origin/main`. **CLOSED 2026-10-27.**

### COVERAGE — stated explicitly

**The count is the STATUS column of the members table, recounted mechanically (the numbers below are
the table's, not an estimate): 88 measured / 101 rule-only.**

* **88 of 189 members carry FULL MEASURED evidence on HEAD** — a repro was compiled and run with this
  lane's `build/bezelc`, and the observed output/diagnostic/abort is the evidence. Breakdown by status:
  `reproduces` 46 + `re-label` 23 + `fixed` 12 + `moved` 6 + `spec-question` 1. Sources: the six
  §3 spot-checks (below), a mechanical sweep over every complete program in every issue body (55
  members), the DONE grid's cells (47 distinct members — the map is below), the two re-derived
  stale premises (#1792, #7108), and the **sixteen members re-derived from the grid's own verdicts**
  in fix-forward 3 (below).
* **101 of 189 carry a TITLE-RULE verdict only** — the mechanism was read from the issue body (which
  quotes the governing stdlib sentence and, for most, the measuring reviewer), but **no repro was
  built**. Breakdown by severity: `other` 66, `crash` 23, `soundness` 12.
  For the ~90 rule-matched members §3 asks to "confirm or re-label", this reading **is** the deliverable:
  each was confirmed in-campaign or re-labelled to another campaign (see the table's `HEAD status`
  column).
* **THE CAMPAIGN'S OWN UNMEASURED BACKLOG IS 38 LIVE MEMBERS, NOT 24.** Of the 101 rule-only rows,
  **38 are LIVE campaign-2 defects** (`reproduces` or `moved` by rule) — they are in scope, their
  mechanism is stated, and **no repro was built for them**. The other 63 rule-only rows are
  re-labelled (55), fixed (3) or spec-questions (5): they leave the campaign or need no repro.
* **THE 38 SPLIT IN TWO, and the split is measured, not asserted:**
  * **13** have **no compilable program in their issue body at all** — a repro must be written from
    scratch. They are #4126, #6254, #6255, #6295, #7098, #7189, #7265, #7274, #7287, #7291, #7319,
    #7331, #7358.
  * **25** have a program in the body that **the sweep's extractor did not turn into a runnable
    module** — MEASURED: the sweep covered 55 members' bodies (60 extracted programs) and **touched
    none of these 25**; each needs the body program rebuilt or a fresh repro.
  * (The earlier "41 rows / 24 live" figure was the narrower list of members whose bodies carry no
    program at all, over BOTH statuses: 41 rows = 20 re-label + 19 live `reproduces` + 1 `fixed`
    (#7329) + 1 `spec-question` (#4481). **24 was wrong: it is 19 live members** — and the backlog
    figure was **52** before fix-forward 3 re-derived the members the grid already measures; it is
    **38** now: **#6648, #6782 and #6809 left the backlog by becoming FIXED**, and **#7293, #7294 and
    #7299 left it by becoming MEASURED** (the grid's own cells measure them, and they reproduce).)
* **66 of the 101 rule-only members are severity `other`** (the §3 "rule-matched" set); those are the
  ones §3 explicitly asks to *confirm or re-label*, and each has been re-labelled or confirmed.
* **THE SIXTEEN MEMBERS RE-DERIVED FROM THE GRID'S OWN VERDICTS (fix-forward 3, 2026-10-28).** The
  review found that **fifteen of the grid's 47 distinct members were still marked `(rule)`** — the grid
  MEASURED them and the table did not say so, which double-counted the grid's own evidence. Each was
  re-derived from its cell's verdict, and from the member's own programs where the cell does not reach
  the member's shape: **#6648, #6782, #6809, #7234, #7271, #7286 and #7296** became `fixed`;
  **#6812, #7245, #7254, #7293, #7294, #7299, #7323, #7336 and #7362** became `measured`. **Every
  count above is the edited table's, recounted mechanically — none is an estimate.**

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

The denominator is the **live** members of each family (the 15 whose mechanism is gone are excluded);
the numerator is the members the family's shared site would close.

| family | live members | B0-leverage count | close rate | falsification verdict |
|---|---|---|---|---|
| **B1** Parse/TryParse/radix | 12 | 10 | **0.83** | **SURVIVES** |
| **B2** ToString(format)/Convert rendering | 21 | 6 | 0.29 | **SPLITS** → B2a (admission) / B2b (rendering contract) |
| **B3** Math/Convert argument typing | 13 | 6 | 0.46 | **SPLITS** → B3a (argument typing) / B3b (widening + evaluation order) |
| **B4** StringBuilder and string member arms | 22 | 6 | 0.27 | **SPLITS** → B4a (admission) / B4b (the `bz_sb_*` runtime contract) |
| **B5** nullable carrier at a builtin position | 7 | 0 | **0.00** | **DOES NOT SURVIVE** — a narrowed `T?` is *legal* (§7.4), so no admission gate rejects it; the rule is campaign 5's unwrap-before-consume |
| **B6** null at a non-nullable builtin parameter | 4 | 4 | **1.00** | **SURVIVES** |
| **B0x** the plan's own B0-named members (symbol-not-name, type-argument arity, ambiguity, constraint, inference, method-group arity) | 11 | 7 | **0.64** | **SURVIVES** |
| **X** re-labelled away (78) + spec-questions (6) | 0 | 0 | — | **not this campaign** — see *Re-labels* |

**#6648 was re-classified on review from Y to N**: `Math.Min/Max/Clamp` over double/int/long have an
UNTYPED RESULT, but **the CALL IS LEGAL** — a parameter-TYPE table does not reach it, so by this
record's own definition it is not a gate rejection.

**The falsification rule's verdict on the PLAN'S B0 CLAIM: it FAILS.** One signature-based admission
gate would have rejected **at most 52 of the 189 members (27.5 %)**, and **at most 39 of the 90 LIVE
defect members (43.3 %)**. Both figures are below one half, so B0 as a single sub-family is **split**.
The anchor is therefore **not one function**; see below.

**THE FIGURE IS AN UPPER BOUND, NOT A MEASUREMENT OF A GATE.** It is a per-member classification —
"would a signature-based admission gate have rejected this call before the arm ran?" — read off each
member's stated mechanism against the stdlib's declared signature. No gate was built and no call was
run through one, so the true figure can only be **lower**. Four of the Y rows were re-classified on
review as **not builtin calls at all** — #6253 and #6254 are USER static/generic classes, #6872 is an
ASSIGNMENT, and #6530 is a type-parameter NAME guard; left as Y they would have read 30.6 % instead of
27.5 %. **The split verdict is unaffected — the true figure can only be lower.**

### Re-labels (78 members — 78 unique, exclusive, every row with a destination)

The table below assigns each of the **78** re-labelled members to **exactly one** destination. The
number of unique ids is 78 and no id appears twice.

| destination | n | members |
|---|---|---|
| campaign 5 — nullable carrier unwrap | 15 | #2620 #2625 #2642 #4585 #5978 #6084 #6116 #6126 #6644 #6646 #6677 #6750 #6873 #7004 #7089 |
| campaign 7 — generic instantiation boundaries | 9 | #6042 #6049 #6181 #6228 #6239 #6307 #6697 #6933 #7066 |
| campaign 8 — owned collections / element lifetime | 9 | #2221 #2647 #2729 #2730 #3509 #3896 #6659 #6892 #6983 |
| campaign 13 — records/statics/contracts/misc (+ the stdlib §4/§5 members outside campaign 2's scope) | 19 | #1765 #2651 #2662 #3943 #4790 #4805 #5961 #6007 #6035 #6056 #6058 #6078 #6106 #6151 #6187 #6751 #6854 #6872 #6881 |
| campaign 1 — using-heads / disposal | 2 | #5355 #7146 |
| campaign 11 — async frames | 1 | #6100 |
| campaign 2 — in-campaign but NOT an admission defect (diagnostic wording, §15.1 tier, enablers, §-citation classes, a report whose premise is wrong — #7254) | 9 | #5409 #5911 #6027 #6029 #7149 #7254 #7308 #7309 #7340 |
| parking-lot (out of the campaign's declared sections; no campaign claims the mechanism) | 14 | #1701 #2094 #2313 #3826 #4255 #4797 #6235 #6689 #6860 #7030 #7090 #7093 #7118 #7246 |

**=> 78 of 189 members (41 %) are not builtin-call-admission defects at all.** The campaign's live
scope is **90 members** (84 `reproduces` + 6 `moved`), not 189.

**THE NINE "IN-CAMPAIGN BUT NOT AN ADMISSION DEFECT" ROWS — SAID ONCE, HERE.** Those nine
(#5409 #5911 #6027 #6029 #7149 #7254 #7308 #7309 #7340) **STAY IN CAMPAIGN 2** — they are not re-labelled
away to another campaign — but they are **not admission defects** (a diagnostic's wording, a §15.1
tier, an enabler, a §-citation class), so they are **NOT counted in the live scope of 90**: the live
scope counts only the members whose symptom still `reproduces` (84) or `moved` (6). They are in the
campaign's 189 and in its §-citation sweep, but they are not in its DONE line.

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

**MEMBERS OF THE PLAN THAT ARE NO LONGER LIVE (fix-forward 3, 2026-10-28).** After the status
re-derivation, four members named in the plan above need no lane: **#6648** (L2), **#6782** and
**#7234** (L3) and **#6809** (L4) are all `fixed` — measured, their mechanism is gone. **The members
table is the authority on what is live**; the plan above is the plan that was made when the scope was
97.

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
**run BY HAND at the gate** today, exactly like its model `scripts/regression_matrix.py`:
`BEZEL_AOT_FORCE_SANITIZE=address scripts/builtin_admission_grid.py --out DIR --bezelc build/bezelc`
compiles every cell with `--sanitize=address` and prints `NATIVE SANITIZER: N check(s) skipped`.
**THE WIRING IS DECIDED (coordinator, 2026-10-28) AND IS A SEPARATE INFRA LANE'S WORK, NOT THIS
LANE'S: the grid's CLASSIFIER stays where it is — under pregate's `python3 -m unittest discover -s
scripts/gate/tests` — and the GRID itself becomes a RATCHET PHASE in `scripts/gate.sh`: a per-cell
baseline that fails ONLY on a violation, the plain leg in the plain cycle, the ASan leg in the
`--full` cycle, and ONE shared phase runner for both campaigns (2 and 5).** Until that lane lands, the
DONE line below is a MANUAL step and "green at every gate" means "green when somebody runs it".

**Cells: 118.** Classes: `OK` / `ACCEPTED` (compiled and ran, but not as the expectation says) /
`REJECTED` (rejected, but not as the expectation says) / `ABORT` (the compiler aborted) / `INTERNAL`
(compiled with an `internal:` note — a silent acceptance inversion) / `LEAK` (the sanitizer reported a
leak) / `CRASH` (the program died — a sanitizer error, a signal, or a non-zero exit). **The last two
are the SANITIZER'S VERDICT, and they are why the instrument is sound under ASan: before fix-forward
3 the `value` branch compared STDOUT ONLY, so a program the sanitizer failed was reported as a clean
pass.**

**BASELINE — cells wrong on HEAD `d9f11d15`, with THIS lane's `build/bezelc` (MEASURED, both legs):**

| leg | cells | wrong | OK | ACCEPTED | REJECTED | ABORT | INTERNAL | LEAK | CRASH | sanitizer |
|---|---|---|---|---|---|---|---|---|---|---|
| plain | 118 | **50** | 68 | 25 | 2 | 8 | 15 | 0 | 0 | `NATIVE SANITIZER: 0 check(s) skipped` |
| `BEZEL_AOT_FORCE_SANITIZE=address` | 118 | **54** | 64 | 24 | 2 | 8 | 15 | 4 | 1 | `NATIVE SANITIZER: 0 check(s) skipped` |

**50 is the campaign's number to drive to zero**, of which **49 are campaign 2's own** and **1 belongs
to campaign 5** (the `math_floor_narrowed` cell — see the campaign split below). The grid reports the
per-campaign split itself (`--campaign 2` / `--campaign 5`). The TSV is **byte-identical across two
different `--out` directories** (the detail column carries cell-relative paths).

**THE TWO LEGS ARE NOT THE SAME RUN, AND THE RECORD MUST NOT SAY THEY ARE.** The class vector agrees on
113 of the 118 cells; the ASan leg adds **4 `LEAK`** cells and **1 `CRASH`**, and the plain leg has
`OK`/`ACCEPTED` where the ASan leg has them. **THE TWO TSVs ARE NOT BYTE-IDENTICAL**: the cell
`user_class_named_stringbuilder` prints `after\n` on the plain leg and prints nothing under ASan (it
SEGVs in `bz_sb_replace` — the builtin runtime run on the user's object). **"Identical on the plain leg
and under ASan" is true of the CLASS VECTOR, not of the TSV.**

**PER-CELL DIFF AGAINST THE PARENT'S GRID (the 113-cell baseline of `f1a72883`):** **5 cells added**
(`int_tostring_radix_null_named` — `OK`; `long_parse_null` — `INTERNAL`; `long_tryparse_null` —
`ACCEPTED`; `uint_parse_null` — `INTERNAL`; `string_isascii_width` — `ACCEPTED`), **none removed**, and
**3 cells changed class, ALL OF THEM IN THE RIGHT DIRECTION**: `int_parse_radix_uint`,
`int_tostring_radix_uint` and `int_tryparse_nullable` went `ACCEPTED` → `OK` (the three cells whose
expectation the coordinator corrected — see *R4*). **Under ASan the same diff shows 4 cells going
`OK` → `LEAK` and 1 going `ACCEPTED` → `CRASH` — that is the B1 classifier change making the
sanitizer's verdict VISIBLE, not a regression: the previous `OK` was the instrument's blind spot.**

**THE CAMPAIGN SPLIT (D13).** Four cells exercise a §7.4-**narrowed** nullable at a builtin
position: `math_floor_narrowed`, `sb_append_narrowed_int`, `int_tostring_radix_narrowed`,
`int_parse_from_narrowed`. **A narrowed `T?` is LEGAL** (§7.4) and §5.1 widens it to `T`, so **no
admission gate rejects these calls** — they are the **unwrap**, which is **campaign 5's** mechanism.
**=> Campaign 2's anchor does NOT include the unwrap.** Those four cells are tagged `campaign = "5"`
in the grid and are excluded from campaign 2's DONE line; the campaign's DONE line is
**"all 114 campaign-2 cells are OK"**, and campaign 5 owns its own four. (If the owner instead folds
the unwrap into campaign 2's anchor, the tag is one constant in the cell table.)

The 8 `ABORT` cells are compiler aborts (`math_floor_nullable`, `math_floor_narrowed`,
`math_floor_nullable_null`, `math_sign_string`, `math_abs_decimal_nullable`, `string_contains_int`,
`string_lastindexof_int`, `decimal_tostring_radix`); the 15 `INTERNAL` cells are the
silent-acceptance inversions (`int_parse_int`, `int_parse_bool`, `int_parse_char`, `int_parse_double`,
`int_parse_object`, `long_parse_intarg`, `long_parse_null`, `uint_parse_null`, `bool_parse_int`,
`char_parse_radix`, `decimal_parse_double`, `double_parse_int`, `string_split_int`, `string_isascii`,
`math_sign_reference`).

**Corpus dedup — RE-RUN on this branch (fix-forward 3, 2026-10-28). ONE RULE, ONE NUMBER SET.**
* **THE CASE FILES.** `tests/conformance/rules/cases/` holds **3693 CASES** — the manifest's
  `tests/conformance/rules/manifest.tsv` has **3693 rows**, and the tree carries exactly those 3693
  ids: **3622 single-file cases** at the top level and **71 multi-module cases** in 71 subdirectories
  (178 `.bzl` files — `main.bzl`, `lib.bzl`, …), **3800 `.bzl` files in all**. "3622 files" is the
  top-level glob alone; "3693" is the CASE count, not a file count. **The rules below are applied to
  every one of the 3800 files.**
* **RULE 1 — BYTE-DUPLICATE.** A cell is a byte-duplicate iff its whole program, normalised (comments
  and whitespace stripped), equals some corpus case's program the same way. **=> 0 cells.**
* **RULE 2 — COVERED.** A cell is covered iff **the cell's OWN CALL EXPRESSION occurs VERBATIM in the
  text of a corpus case file**. The cell's own call expression is the call under test, extracted
  mechanically from the cell's own statements: a cell that declares a `subject` contributes the call
  whose callee's last segment equals the subject's last segment; a cell that declares none contributes
  the first call in its statements that is not the harness's `Console.WriteLine(...)` wrapper, not a
  `new X(...)` constructor and not a readback of a local the cell itself binds; a method-reference cell
  contributes the bound member name. **=> 9 cells are COVERED and 109 are NEW** (of 118):

  | cell | its own call expression | corpus cases |
  |---|---|---|
  | `int_parse_radix_ok` | `int.Parse("ff", 16)` | `l9_1_0011` |
  | `int_tostring_radix` | `(255).ToString(16)` | `l9_1_0008` |
  | `int_tostring_format_x` | `(255).ToString("x")` | `l2_18_0012` |
  | `double_tostring_format_c` | `(1.5).ToString("C")` | `l2_18_0017` |
  | `int_tostring_radix_narrowed` | `n.ToString(16)` | `l9_1_0013` |
  | `encoding_getstring_int_array` | `Encoding.UTF8.GetString(a)` | `s22_6_0102` |
  | `encoding_getstring_byte_array` | `Encoding.UTF8.GetString(a)` | `s22_6_0102` (textually identical to the row above — the same expression, so the same hit) |
  | `math_max_reference` | `Math.Max` | `l2_2_0011`, `l2_2_0014`, `s14_5_0017`–`0023`, `s22_5_0018`, `s27_6_0013`–`0020`, `s5_1_0132`/`0133` — **17 cases** |
  | `math_sign_reference` | `Math.Sign` | `l2_2_0004`, `l2_2_0010`, `l2_2_0012`, **`l2_2_0013`** |

* **THE OLD FIGURES ARE REPLACED, NOT ADDED TO.** The round-1 record's "3622 case files", "6 covered",
  "106 NEW" and "57 covered / 55 new" were three different file counts and two different rules; and
  the id `l2_2_0022` was wrong — it holds `Math.Round`, not `Math.Sign` (the fourth `Math.Sign` case is
  **`l2_2_0013`**). **There is no second, looser rule here: ONE rule, ONE number set.** (The earlier
  "17 loosely covered, 93 new, 3693 files" line is gone for the same reason.)

### The grid's own fixes (review round 1) — what changed and why

| cell | old expectation | new expectation | evidence |
|---|---|---|---|
| `int_tostring_radix_string` | `reject` | **`fault` / `FormatException`** | stdlib §2.18 declares `ToString(format: string)` for the primitives and "16" is not in the integer vocabulary (D/Dn/X/Xn/B) ⇒ the call is ADMITTED and the specifier is unrecognized at RUN time ("An unrecognized specifier raises `FormatException`"). The banked corpus case `l2_18_0027` (`expect: run`) confirms it. The compiler today prints "255" — it binds the string to the RADIX path — so the DEFECT is real and the cell stays wrong, but under the old expectation it was wrong under EVERY compiler behaviour. |
| `string_isascii` | `value` / `True` | **`reject`** | stdlib §9.6 declares `public pure readonly property IsAscii: bool` — a PROPERTY. `"abc".IsAscii()` has no candidate ⇒ §22.6 compile error. |
| `string_isascii_property` | — | **NEW cell**: `value` / `True` | the legal property read `"abc".IsAscii` (measured: prints `True`). |
| `debug_writeline_stringbuilder` | `reject` (green for the wrong reason) | **`reject`, import `System.Diagnostics`** | the old cell imported `System`, so `Debug` was undeclared and the cell was green on "`Debug` is declared in `System.Diagnostics`". With the right import the call COMPILES and writes an empty line — that is **#6780**, a live member. |
| `string_remove_oob` | `noabort` (vacuous: rc 70 was OK) | **`fault` / `IndexOutOfRangeException`** | stdlib §9.6: "a `startIndex` above `Length` … raises `IndexOutOfRangeException`". Measured: `"ab".Remove(253)` raises it. **=> #7108's mechanism is GONE** (fixed by `c0c9e3f9`, *fix #7168*, in `origin/main`) — the tracker entry is stale. |
| the `noabort` kind | `nocrash` (ignored the exit code) | **renamed `noabort`, and it now requires rc == 0** | a runtime FAULT is a wrong answer, not a pass. The 3 remaining `noabort` cells (`math_abs_decimal` — now a `value` cell, `sb_appendformat_string`, `sb_append_narrowed_int`) all exit 0 today. |
| **all 60 reject cells** | `want = None` ("any error will do") | **every reject cell now NAMES its needle** | D5: the `C()` helper RAISES if a reject cell has no needle, so the shape cannot regress. |
| **all 60 reject cells (review round 2)** | **ONE guessed wording each** (21 transcribed from the compiler's own wording, **39 predicted**) | **every reject cell now lists the DIAGNOSTIC FAMILIES it accepts** (2–11 alternatives each today, built by the `F_*` family helpers) | **N1, the last gap to the DONE instrument.** A fix that lands on a different wording of the SAME family must still turn the cell green, and a fix that lands on a DIFFERENT family must turn it red. The `C()` helper now also RAISES if a reject cell lists fewer than two alternatives. |
| **the two bare needles** | `"no matching overload of"` (6 cells), `"type argument"` (2 cells) | **member-specific**: `F_overload("int.Parse")` → ``no matching overload of `int.Parse` ``, ``no applicable overload of `int.Parse` ``, ``no matching overload for `int.Parse` ``; `F_typearg("Console.WriteLine")` → ``explicit type argument(s)``, ```Console.WriteLine` declares no type parameter`, ```Console.WriteLine` is not a generic method``` | a bare family head matches a diagnostic about a DIFFERENT call in the same program (the D3 failure mode). |
| the brittle needles | `"supplies 1 argument(s) but stdlib §3.1 declares"` (exact count + section) | `F_arity` lists four: the counted form, the section-pinned-but-uncounted form, the member-qualified form, and the `expects N argument(s)` rewording | a reworded arity message must not turn a correct fix into a red cell. |
| — | the TSV had no subject column | **every cell names its `subject`** (the built-in member it is about) | the subject is what makes a diagnostic about another call unable to satisfy the cell; the self-test proves it. |
| — | — | **NEW: `imported_user_class_stringbuilder_qualified` and `imported_user_class_stringbuilder_bare`** | **N5, the cross-module face.** See *The cross-module face* below. |
| `math_abs_float` | — | **NEW cell**: `reject` | **R2 (coordinator, 2026-10-28).** stdlib §2.2 declares four `Abs` overloads (int/long/double/decimal); §5.1 gives `float` → `double` **and nothing else**, and puts `decimal` ↔ the binary floats on its never-implicit list — so a `float` argument converts to exactly ONE parameter type and the call must resolve to `Abs(double)` and return `double`. `var f: float := Math.Abs(-1.5f);` must therefore be **REJECTED** (`double` → `float` never narrows implicitly). MEASURED on HEAD: the compiler types the result `float` and ACCEPTS it — that is #7061. |
| `math_abs_uint` | `reject` (unchanged) | **comment added; the cell STANDS** | **R2.** `uint` converts to THREE of the four declared parameter types (`long`, `double`, `decimal`) and to NONE of them exactly, so §22.6 makes the call ambiguous and the rejection is CORRECT. **#7254 IS NOT A DEFECT** — this cell is now the guard against a lane "fixing" a correct rejection. |
| `int_parse_radix_uint` | `reject` | **`value` / `255`** | **R4 (coordinator, 2026-10-28).** §5.1 (v0.345)'s constant narrowing is VALUE-BASED and crosses SIGNEDNESS, so a `uint`-typed CONSTANT radix reaches the `int` parameter. MEASURED on HEAD: the compiler compiles, runs and prints `255` — it was RIGHT and the cell was wrong. |
| `int_tostring_radix_uint` | `reject` | **`value` / `ff`** | **R4.** The same rule: `(255).ToString(16u)` compiles, runs and prints `ff` — the RADIX form, which is what stdlib §9.1 requires of `ToString(radix)`. |
| `int_tryparse_nullable` | `reject` | **`value` / `16`** — **and the cell's PROGRAM changed** (it now prints `v`) | **R4.** §7.4 (v0.345) narrows a local "from a declaration whose initializer is such an expr — `var x: T? := expr;`", so `var s: string? := "16";` is narrowed non-null and `s` reaches the `string` parameter; §5.1's own wording makes "un-narrowed" load-bearing. MEASURED on HEAD: the compiler compiles, runs and prints `16`. **A `value` cell is graded on the program's stdout, so the body had to start printing — this cell was NOT a one-line expectation change.** |
| `string_isascii_width` | — | **NEW cell**: `value` / `True` | #7245's own shape, which `string_isascii_property` does not reach: the property is defined by the CONTENT (stdlib §9.6, "true iff every code point < 128"), and a one-code-point slice of `"a" + "日"` holds only 'a'. MEASURED on HEAD: the compiler prints `False`. |
| `string_repeat_string` | value "ababab" (unchanged) | **comment added; the cell STANDS** | **R3 (coordinator, 2026-10-28).** stdlib §9.6 declares `Repeat(c: char, count: int)` and `Repeat(unit: string, count: int)` side by side; the declared overload MUST BE IMPLEMENTED, and the gate may not reject a declared overload. The compiler's `char`-only surface is a defect (#7129, #7277), not a contract. |
| — | — | **NEW: the grid carries the CELL→MEMBER map** (`MEMBER` in `scripts/builtin_admission_grid.py`) | the record's cell→member table is now machine-checkable: the module raises if a cell has no entry or an entry names no cell, and the self-test checks both. |
| every cell | the TSV's detail column embedded the ABSOLUTE program path | **cell-relative path** | two runs into different `--out` directories now produce **byte-identical** TSVs (verified). |
| — | the `clean` expectation kind was dead code | **removed** | — |

### The reject needles are FAMILIES, not one guessed wording (N1)

**THE PROBLEM (measured by the reviewer, confirmed here).** A reject cell that names ONE wording is a
ratchet that turns RED on a *correct* fix that words the same diagnostic differently, and — if the
wording is a bare family head — a ratchet that can turn GREEN on a diagnostic about a **different**
call in the same program. The compiler has **at least three** "no such member" families:
``string member `.M` is not yet supported in codegen``, ``'T' has no static method or property 'M' (§3.4)``,
``type 'T' has no static method or property 'M' (§3.4)``.

**THE FIX.** Every reject cell's needle is now a **LIST** — the diagnostic FAMILIES a correct fix may
use — built by the family helpers in `scripts/builtin_admission_grid.py`:

| family | the alternatives it offers | the compiler's own wording it is transcribed from |
|---|---|---|
| `F_overload(subject)` | ``no matching overload of `X` ``, ``no applicable overload of `X` ``, ``no matching overload for `X` `` | `ExprTypecheck_Calls.cpp` (`"no matching overload of `" + head + "."`) |
| `F_convertible(subject)` | `not implicitly convertible to the target type`, ``the argument to `X` has a type that is not implicitly convertible to the target type`` | `ExprTypecheck.cpp` (§5.1) |
| `F_nullable(subject)` | `is nullable but the target type is non-nullable`, ``the argument to `X` is nullable but the target type is non-nullable`` | `ExprTypecheck.cpp` (§7) |
| `F_arity(subject, §, N)` | the counted form, the section-pinned form, the member-qualified form, the `expects N argument(s)` rewording | `ExprTypecheck_Calls.cpp` (`"the call to `StringBuilder.M` supplies N argument(s) but stdlib §3.1 declares"`, and the §9.6 string form) |
| `F_ambiguous(subject)` | ``ambiguous call to `X` ``, ``ambiguous call to X``, `` `X` is ambiguous`` | `ExprTypecheck_Calls.cpp` (`overloadAmbiguityMessage`) |
| `F_dispatch(subject)` | ``cannot dispatch member 'M' ``, ``member `M` on `string` is not yet supported in codegen``, ``string member `.M` is not yet supported in codegen`` | `LlvmBackend_Calls2.cpp` (the dispatch backstop) and `ExprTypecheck_Calls.cpp` |
| `F_no_such_member(receiver, member)` | the three "no such member" wordings | `ExprTypecheck_Calls.cpp` |
| `F_property(subject)` | `` `M` is a property``, ``property `M` `` | — (the property-vs-method shape) |
| `F_typearg(subject)` | `explicit type argument(s)`, `` `X` declares no type parameter``, `` `X` is not a generic method`` | `ExprTypecheck_Calls.cpp` (§14) |
| `F_inference(subject)` | `inconsistent type-argument inference`, ``no matching overload of `X` `` | `ExprTypecheck_Calls.cpp` (§14.1) |
| `F_generic_ref(subject)` | `is a reference to a generic method`, `` `X` is a reference to a generic method``, `` `X` names no type argument`` | `ExprTypecheck_Calls.cpp` |
| `F_initializer(subject, v)` | `the initializer for`, ``the initializer for `v` has a type that is not implicitly convertible to the target type`` | `ExprTypecheck.cpp` |
| `F_ambiguous_name(subject)` | ``bare name `M` is ambiguous``, `is ambiguous: an imported public from`, `shadows the always-available type` | `ExprTypecheck_Calls.cpp` (§2.3) |

**WHERE THE "NO SUCH MEMBER" FAMILIES ARE AND ARE NOT ACCEPTED — and why.** A fix can only say "no
such member" where the called FORM has no member at all. **MEASURED on HEAD: not one of the 63 reject
cells produces a "no such member" diagnostic** (the member exists in the form it is called), so the
three no-such-member families are accepted **only** by `string_isascii` — whose method spelling of a
PROPERTY really has no member. For the other 62 the member exists, so the plausible families are the
overload / conversion / nullability / arity ones, and those are what the cells accept.

**THE RESIDUAL RISK, STATED.** A dozen family phrases carry **no member name** (the compiler's own
wording for them has none) — they are listed in `CONTEXT_FREE` in the grid and are the only
alternatives that are not anchored to the cell's subject. **They are safe here because each cell's
program has exactly one call under test and its only other call is a `Console.WriteLine` of that
call's own result** — a type the stdlib fixes, so no §5.1 or §7 diagnostic can arise at the outer
call. The self-test proves the complementary half mechanically: **no anchored alternative of any cell
is satisfied by a diagnostic about another call in the same program.**

### The cross-module face (N5)

**The gap the reviewer named: no cell covered a class named like a builtin IMPORTED from another
module.** The imported-name resolution is a distinct path (cf. #5313/#5355's imported-name family).
**MEASURED HERE (not asserted), on HEAD `d9f11d15`, with a two-module build (`bezelc -c lib.bzl -o
lib.o` then `bezelc --import lib.o app.bzl`):**

* the **qualified** spelling `Lib.StringBuilder` calls the **user's own** `Replace` — the program
  prints `USER-REPLACE` then `after`, **rc 0**. **The imported shape is ALREADY SAFE.**
* the **bare** spelling `StringBuilder` (with `import Lib;` in scope) is **REJECTED by sema**:
  ``bare name `StringBuilder` is ambiguous: an imported public from `Lib` shadows the always-available
  type; qualify it via a module-qualified name (e.g. `Lib.StringBuilder`) or an import alias
  (§2.3 / §2.2, v0.309; fix #5140)``.

**Both are now CELLS, not prose** — a cell that pins an already-correct behaviour is worth more than
a note: `imported_user_class_stringbuilder_qualified` (expects `value` `USER-REPLACE\nafter\n`) and
`imported_user_class_stringbuilder_bare` (expects `reject`). **Both are `OK` on HEAD**, so the
baseline's wrong-cell count is unchanged; the grid now fails if the imported path regresses.

### The cell→member map

**The grid carries the map** (`MEMBER` in `scripts/builtin_admission_grid.py`): every cell names the
tracker member it covers, or `''` for a CONTROL cell (the legal spelling of a call the tracker does
not name). The module **raises** if a cell has no entry or an entry names no cell, and the self-test
checks both. **79 of the 118 cells cover a tracker member (47 distinct); 39 are controls.**

| member | cells |
|---|---|
| #6635 | `string_contains_int`, `string_lastindexof_int` |
| #6636 | `string_split_int` |
| #6647 | `math_abs_decimal_nullable` |
| #6648 | `math_minmax_typed_result` |
| #6755 | `object_holding_stringbuilder` |
| #6780 | `debug_writeline_stringbuilder` |
| #6782 | `datetime_tostring_null` |
| #6809 | `sb_insert_two_strings`, `sb_insert_zero`, `sb_remove_two_args`, `sb_replace_int` |
| #6812 | `sb_append_null`, `sb_append_null_literal` |
| #6825 | `long_parse_null`, `long_tostring_null`, `long_tryparse_null`, `uint_parse_null` |
| #6826 | `string_contains_object`, `string_replace_object` |
| #6864 | `math_max_uint` |
| #6865 | `math_min_char` |
| #6884 | `math_max_mixed` |
| #6942 | `math_max_reference`, `math_sign_reference` |
| #6957 | `encoding_getstring_int_array` |
| #7061 | `math_abs_float` |
| #7108 | `string_remove_oob` |
| #7129 | `string_repeat_string` |
| #7173 | `builtin_typearg_arity`, `builtin_typearg_list` |
| #7180 | `string_compareto_named_reordered`, `string_equals_named_ordered`, `string_equals_named_reordered` |
| #7217 | `imported_user_class_stringbuilder_qualified`, `user_class_named_stringbuilder` |
| #7227 | `math_sign_bool`, `math_sign_string` |
| #7234 | `convert_tostring_char`, `convert_tostring_uint` |
| #7239 | `convert_tostring_radix_long` |
| #7242 | `math_sqrt_uint` |
| #7244 | `sb_appendformat_string` |
| #7245 | `string_isascii_property`, `string_isascii_width` |
| #7254 | `math_abs_uint` |
| #7271 | `int_tostring_radix_uint` |
| #7284 | `decimal_tostring_radix` |
| #7285 | `int_parse_radix_bool`, `int_parse_radix_uint` |
| #7286 | `int_parse_radix_string`, `int_tostring_radix_string` |
| #7290 | `int_tryparse_int` |
| #7293 | `double_parse_int`, `double_tryparse_int` |
| #7294 | `int_parse_double`, `int_parse_int`, `int_parse_object` |
| #7295 | `char_parse_radix` |
| #7296 | `int_tostring_radix_null`, `int_tostring_radix_null_named` |
| #7297 | `math_floor_narrowed`, `math_floor_nullable`, `math_floor_nullable_null` |
| #7299 | `bool_parse_int`, `bool_tryparse_int`, `char_tryparse_int`, `int_parse_bool`, `int_parse_char` |
| #7300 | `decimal_parse_double`, `decimal_tryparse_double` |
| #7303 | `double_tostring_format_bad`, `double_tostring_format_c` |
| #7323 | `int_tryparse_double` |
| #7333 | `bool_tostring_format` |
| #7336 | `char_tostring_format` |
| #7359 | `int_tostring_format_X`, `int_tostring_format_x` |
| #7362 | `double_tostring_format_d` |

**79 of the 118 cells cover a tracker member (47 distinct); the other 39 are CONTROL cells** — the
legal spelling of a call the tracker does not name (their program must still compile and print what
the cell says). The authoritative list is the `MEMBER` dict in the grid;
`scripts/builtin_admission_grid.py --list` prints `cell  member  subject` for every cell, and the
TSV carries a `member` column.

### The grid's self-test (D11)

**`scripts/gate/tests/test_builtin_admission_grid.py`** — pregate runs
`python3 -m unittest discover -s scripts/gate/tests`, so this is picked up automatically. It is
**build-free**: a fake `bezelc` (a shell script) stands in for the compiler and for the generated
program, so the generator, the classifier and the cell table are exercised end to end in ~0.6 s.
**Twenty-four tests, all green** (the whole discovery: 1063 tests, OK, 2 skipped):
* every `reject` cell lists **at least two** diagnostic alternatives (D5 + N1, structural);
* every `reject` cell names its **subject** (structural);
* every reject alternative is **anchored** (it names the cell's own subject, its head or its
  member) **or** is one of the compiler's context-free family phrases — a bare phrase such as
  "no matching overload of" would match a diagnostic about a DIFFERENT call (the D3 failure mode);
* **no diagnostic about another call in the same program satisfies any cell** — the false-OK mirror
  of D3, proved over every cell's decoys;
* **the classification is STABLE within a family**: every alternative of every reject cell turns that
  cell `OK` (this is the perturbation test the reviewer asked for: a fix that words the same
  diagnostic differently still turns the grid green);
* **a diagnostic in NO family leaves the cell `REJECTED`** — the cell is a false wrong only when the
  diagnostic is genuinely absent;
* the cell names are unique, generation is deterministic, and every campaign tag is known;
* every cell has a `MEMBER` entry and every entry names a cell (the map's completeness);
* the cross-module cells exist and really import a second module;
* `reject` is `OK` **only** when the diagnostic contains one of its needles, and `REJECTED` (not
  `OK`) when it does not;
* a compiler abort and a compiler **signal** are the `ABORT` class;
* an `internal:` degrade is the `INTERNAL` class;
* a `value` cell checks the **exact** output;
* a `noabort` cell is `ACCEPTED` (not `OK`) when the program faults at run time (D4);
* a `fault` cell needs the fault text;
* the row's program path is cell-relative (D12);
* **the classifier's SEVEN classes**, including the two the model has and this grid did not: **`LEAK`**
  (the sanitizer reported a leak) and **`CRASH`** (the program died) — a program the sanitizer fails
  is never a clean pass;
* **a `value` cell needs `rc == 0` as well as the exact stdout** (the branch compared stdout only);
* the cell's program runs under the house oracle's `ASAN_OPTIONS=detect_leaks=1:exitcode=99`;
* **the reject cell set is PINNED — the count AND the names** — so a cell whose expectation KIND
  changes turns this suite RED (A2).

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

**R2 AND R3 ARE RULED — by the coordinator, on 2026-10-28.** The rulings are recorded here with the
arithmetic that decided them, because the record's reasoning is the campaign's reasoning: a ruling
without its arithmetic is a ruling the next lane cannot check.

### R2 — the stdlib's declared signature is authoritative (RULED 2026-10-28)

**THE ANSWER: YES — AND THERE IS NO SEPARATE "ACCEPTANCE-VS-DECLARATION" QUESTION TO ANSWER. A BUILTIN
CALL IS RESOLVED AGAINST THE DECLARED OVERLOAD SET, WITH §5.1's CONVERSION TABLE AND §22.6's RANKING.
WHERE THE COMPILER'S CURRENT BEHAVIOUR CONTRADICTS THAT RESOLUTION, THE COMPILER IS WRONG.**

The reason there is no separate question is that **the declared set and the conversion table together
decide each case — and they decide them DIFFERENTLY, which is exactly what the two live cases turn
on.**

**stdlib §2.2 (v0.345) declares exactly FOUR `Math.Abs` overloads:**
```
public static pure method Abs(x: int): int;
public static pure method Abs(x: long): long;
public static pure method Abs(x: double): double;
public static pure method Abs(x: decimal): decimal;
```
**There is no `float` overload and no `uint` overload.** And **spec §5.1 (v0.345)'s table says:**
* `float` → `double` — **and nothing else**;
* `uint` → `long`, `ulong`, `float`, `double`, `decimal`;
* **"Between `decimal` and binary floats (`float`, `double`)" is on §5.1's NEVER-IMPLICIT list** — so
  `float → decimal` does **not** exist, and `float → int` / `float → long` do not exist either
  (nothing narrows implicitly).

**=> #7061 (`Math.Abs(float)`): THE COMPILER IS WRONG. IT MUST RESOLVE TO `Abs(double)` AND RETURN
`double`.** A `float` argument converts to **exactly one** of the four parameter types (`double`) —
`float → decimal` is never implicit and `float → int`/`float → long` do not exist — so **there is no
ambiguity and no rejection: there is ONE candidate, and the result type is `double`.** **=> #7061 IS
A DEFECT**, and the grid's new cell `math_abs_float` pins it: `var f: float := Math.Abs(-1.5f);` must
be **REJECTED**, because `double → float` never narrows implicitly (§5.1). MEASURED on HEAD
`d9f11d15`: the compiler types the result `float` and **accepts** that assignment — that is #7061.

**=> #7254 (`Math.Abs(4000000000u)`): THE COMPILER IS RIGHT. THE CALL IS AMBIGUOUS AND MUST BE A
COMPILE ERROR (§22.6).** A `uint` argument converts to **three** of the four parameter types — `long`,
`double` AND `decimal` — and to **none** of them exactly, so §22.6's ambiguity rule applies.
**=> #7254 IS NOT A DEFECT: THE RECORD SAYS SO EXPLICITLY, SO THAT NO LANE "FIXES" A CORRECT
REJECTION AND BREAKS THE GRID.** (The neighbouring sentence stdlib §2.3 gives for `Convert.ToString` —
"A `char` and the integer types other than `int` and `long` each convert to two or more of these
parameter types and match none exactly, so the call is ambiguous and a compile error (§22.6)" — says
the same about the same parameter-type family, so the grid's `convert_tostring_char` and
`convert_tostring_uint` cells STAND as REJECTs.) **#7254 is therefore re-labelled OUT OF THE LIVE
scope** (it was `reproduces`, now `re-label`). **THE LIVE SCOPE IS 90 TODAY** (84 `reproduces` +
6 `moved`): it was 98 before #7254's re-label and 97 after it, before fix-forward 3 re-derived the
sixteen members the grid measures.

### R3 — `string.Repeat(unit: string, count: int)` (RULED 2026-10-28)

**THE ANSWER: THE DECLARED OVERLOAD MUST BE IMPLEMENTED. THE GATE MUST NOT REJECT A DECLARED
OVERLOAD.**

**stdlib §9.6 (v0.345) declares BOTH, side by side:**
```
public static pure method Repeat(c: char, count: int): string;      // count < 0 raises
public static pure method Repeat(unit: string, count: int): string; // same count contract; Repeat("", n) = ""
```
**The compiler implements only the `char` overload and rejects the `string` one (#7129, #7277). THAT IS
A DEFECT IN THE COMPILER, NOT A CONTRACT TO BE ENFORCED.** The compiler's current `char`-only surface
does not become the contract by being what the compiler happens to do. **=> #7129 AND #7277 ARE FIXES
(IMPLEMENT THE DECLARED OVERLOAD), NOT REJECTIONS — AND THE GRID CELL `string_repeat_string`, WHICH
EXPECTS THE VALUE "ababab", STANDS.**

### R4 — a `uint` CONSTANT radix, and a §7.4-NARROWED local (RULED 2026-10-28)

**THE ANSWER: BOTH CALLS ARE ADMISSIBLE. THE COMPILER IS RIGHT AND THE GRID'S EXPECTATIONS WERE
WRONG.** Three of the grid's cells asserted a rejection the spec does not ask for; the compiler admits
all three, and is right to. **The anchor lane reached the same conclusion independently**, and its two
control rows — `int_parse_with_a_uint_constant_radix_is_admitted` and
`int_tryparse_with_a_narrowed_local_is_admitted` — **AGREE** with the measurements below.

**THE `uint` CONSTANT RADIX — spec §5.1 (v0.345), "Constant narrowing", verbatim:**
> The rule is **value-based, not type-based**: the source may be a compile-time constant of **any**
> integer type — not only `int` — and the target any integer type whose range contains its value,
> **across signedness** (`const N: long := 8; var b: byte := N;` is well-typed; a `uint`-typed constant
> `7` is admissible where `int` is expected); for non-constant expressions the conversion table and
> the never-implicit list above govern unchanged.

**=> `int.Parse("ff", 16u)` AND `(255).ToString(16u)` ARE ADMISSIBLE**, and the #7285 fix landed
DELIBERATELY admits them (its own test row is `uint_literal_radix_still_parses_hex`). **MEASURED on
HEAD `d9f11d15`: `int.Parse("ff", 16u)` compiles (rc 0), runs (rc 0) and prints `255`;
`(255).ToString(16u)` compiles, runs and prints `ff` — the RADIX form, which is what stdlib §9.1
requires of `ToString(radix)`.** Both cells' expectations were flipped to `value`.

**THE NARROWED LOCAL — spec §7.4 (v0.345), verbatim:**
> … and **from a declaration whose initializer is such an `expr` — `var x: T? := expr;` or
> `const x: T? := expr;`** ([§21.1](#sec-variable-declarations)) — which is the declaration's
> assignment, its narrowing invalidated by exactly the forms below;

**and §5.1's own wording makes "un-narrowed" load-bearing (verbatim):**
> … a nullable `T?` is none of these kinds: by the lifting rule above it converts at most to
> `Object?`, never to `Object` ([§7.1](#sec-non-nullable-and-nullable-types)), **so an un-narrowed
> `Animal?` is rejected at an `Object` parameter** as it is at an `Animal` one;

**=> `var s: string? := "16";` NARROWS `s` NON-NULL**, and a read of a binding §7.4 has narrowed
non-null is of its non-nullable type (§22.6) — **so `int.TryParse(s, out v)` IS ADMISSIBLE. MEASURED
on HEAD `d9f11d15`: it compiles (rc 0), runs (rc 0) and prints `16`.** The cell's expectation was
flipped to `value`, and **its PROGRAM also changed** — a `value` cell is graded on the program's
stdout, so the cell now prints `v` (it used to discard the parse result and print nothing). It is the
anchor lane's control row `int_tryparse_with_a_narrowed_local_is_admitted` **verbatim**: the same
program, the same expected stdout.

**WHAT THIS RULING DOES NOT TOUCH.** **No compiler file was changed** — the compiler was right in all
three cases, and the changes are grid expectations only. **And NO GRID CELL PINS THE CODEGEN'S
REFUSAL OF A NARROWED `int?` RADIX**: that is a different shape from the three above — `int.Parse("ff",
radix)` over `var radix: int? := 16;` is REFUSED, while the POSITIONAL `ToString` site admits the same
narrowed value. The refusal lives in `src/codegen/LlvmBackend_Calls2.cpp` (`parseRadixBinding`), which
consults the nullable type and not §7.4's narrowing facts; **it is not this lane's file, it is not the
anchor's file, and it is not a grid question — it is the coordinator's.** No cell asserts it, so the
grid enforces no contradiction in either direction.

### The rule the anchor implements, in one sentence

**THE STDLIB'S DECLARED SURFACE IS THE CONTRACT. THE GATE MAY NOT REJECT A CALL THE STDLIB DECLARES,
AND MAY NOT ACCEPT A CALL THE STDLIB DOES NOT DECLARE — EXCEPT WHERE THE SPEC SAYS OTHERWISE.**

**WHAT THE RULINGS DID TO THE GRID: NOTHING HAD TO CHANGE.** The grid already pre-committed to both
answers, so no cell's expectation contradicts a ruling:
* **R2** — `math_abs_uint` expects a **REJECT** ("ambiguous call to `Math.Abs`", the wording §22.6's
  ambiguity rule produces) and it **STANDS**; `convert_tostring_uint` and `convert_tostring_char`
  expect a **REJECT** ("ambiguous call to `Convert.ToString`", stdlib §2.3's ambiguity note) and they
  **STAND**.
* **R3** — `string_repeat_string` expects **value "ababab"**, i.e. the DECLARED
  `Repeat(unit: string, count: int)` overload must WORK (§9.6), and it **STANDS**.
* **ONE CELL WAS ADDED** for R2's first half: `math_abs_float` (expects a REJECT — the result type
  must be `double`), and it is the **only** cell whose expectation the rulings created.

**§3 says campaign 2 has "none hard". THAT IS TRUE, AND IT IS NOW PROVEN RATHER THAN ASSUMED:** the
diagnostic half and the severity half are settled by existing authorities, R2 and R3 are **RULED**,
and no ruling blocks the anchor. **NO RULING IS OUTSTANDING.**

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
* **Rows:** 189 members re-measured (88 with full MEASURED evidence; the 101 others by title rule, and
  the unmeasured list is named above). 118 grid cells built and run twice (plain + ASan).
* **Fail-without proof:** not applicable — this lane changed no compiler behaviour, so there is no
  behaviour to prove fail-without. The instrument's *baseline* is the fail-without for the lanes that
  follow: each lane's cell must go from its entry CLASS to `OK` with the fix and stay at its entry
  CLASS without it.
* **Matrix result:** `scripts/builtin_admission_grid.py` — **118 cells, 50 wrong** on HEAD (49 of them
  campaign 2's own, 1 campaign 5's), `NATIVE SANITIZER: 0 check(s) skipped` in both legs, and the TSV
  is byte-identical across two different `--out` directories. **The two legs are NOT the same run** —
  the ASan leg adds 4 `LEAK` cells and 1 `CRASH` (see *DONE instrument*). **Measured on this
  lane's `build/bezelc` at `d9f11d15`** — `origin/main` has since moved to `9be24022`, whose four
  commits touch **no `src/` file** (`git diff --stat d9f11d15 origin/main -- src/` is empty), so the
  compiler and the baseline are unchanged.
* **Self-test:** `scripts/gate/tests/test_builtin_admission_grid.py` — 24 tests, build-free, green;
  pregate runs it via `python3 -m unittest discover -s scripts/gate/tests` (the whole discovery:
  1063 tests, OK, 2 skipped).

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

**FIX-FORWARD (review round 2, ACCEPT-W-NOTES).** The reviewer confirmed every number and the
perturbation test, but found **seven defects, one of them blocking the DONE instrument** (N1: 39 of
the 60 reject needles were PREDICTED single wordings, so a correct fix landing on a different wording
of the same family turned the cell RED). All seven are fixed. **THE CHANGED FILES ARE
`scripts/builtin_admission_grid.py`, `scripts/gate/tests/test_builtin_admission_grid.py` and the two
documentation files** — and the changed cells are:
* **EVERY ONE OF THE 60 REJECT CELLS** now carries a LIST of the diagnostic families it accepts
  (2–11 alternatives each today, built by the `F_*` helpers) — the per-cell lists are in
  *The reject needles are families* above, and every list is in the grid's cell table;
* the two bare needles (`"no matching overload of"`, `"type argument"`) are gone — member-specific now;
* the brittle arity needles now list four wordings;
* `imported_user_class_stringbuilder_qualified` and `imported_user_class_stringbuilder_bare` are NEW
  (the cross-module face, N5) — both `OK` on HEAD;
* **THE CELL COUNT MOVED 110 → 113 AND THE TSV GAINED TWO COLUMNS** (`subject`, `member`).
  **THE BASELINE WAS 113 cells, 49 wrong** (48 campaign 2 + 1 campaign 5) — superseded, see *DONE
  instrument* above for the current 118 cells / 50 wrong: the rewrite of the needles
  changed **no** cell's class (a per-cell diff against the previous baseline says so), and the ONE new
  cell — `math_abs_float`, added for the coordinator's R2 ruling — is the 49th wrong cell (it is
  `ACCEPTED` today: the compiler types `Math.Abs(-1.5f)` as `float`).
  **=> THE L0 ANCHOR LANE MUST RE-READ THE GRID AND RE-MEASURE ONCE MORE.**
* the L1 lane's commit `02e0d371` (the codegen receiver discriminator) turns
  `user_class_named_stringbuilder` from `ACCEPTED` to `OK`; **on a tree that carries it the baseline
  is 48 wrong, not 49** (the L1 lane measured 47 of 110 cells when the grid had 110 cells and no
  `math_abs_float`) — that measurement is the L1 lane's, not this lane's: **this lane's numbers are
  measured on THIS lane's `bezelc` at `d9f11d15`**, and the fix changes nothing in the needle sets.
* **THE B0 LEVERAGE COUNT — the number this lane was sent to produce: AT MOST 52 of the 189 members
  (27.5 %), and at most 39 of the 90 LIVE defect members (43.3 %).** Both are below one half ⇒
  **B0 splits**; the anchor is a table + two admission points, not one function, and B2, B3, B4 each
  split into an admission half and a non-admission half. The figure is an **UPPER BOUND** (a
  per-member classification, not an execution of a gate) and it can only fall.

**FIX-FORWARD 3 (review round 3, ACCEPT-W-NOTES BUT NOT PICKABLE AS-IS).** The reviewer accepted the
record and its numbers but found **four blockers and three advisories**, all of them inside the same
four files. **B1 — the ASan leg never read the sanitizer's verdict** (fixed: the grid now carries the
model's `LEAK` and runtime-`CRASH` classes, a `value` cell requires `rc == 0` as well as the exact
stdout, and the program runs under the house oracle's `ASAN_OPTIONS=detect_leaks=1:exitcode=99`; the
skip counter was RIGHT and is kept). **B2 — the record contradicted itself on the live scope** (fixed:
97, and the stale 98/92 is gone). **B3 — the corpus-dedup numbers were mutually contradictory and not
reproducible** (fixed: ONE rule, ONE number set, above). **B4 — the members table contradicted the
grid's own measurements** (fixed: sixteen members re-derived, the live scope restated at 90, and the
unmeasured backlog at 38). **A1 — the reject-cell count and the alternatives range were wrong** (fixed:
63 cells, 2–11 alternatives). **A2 — the self-test could not go RED when a cell's expectation KIND
changed** (fixed: the reject set is pinned by count AND by name). **A3 — the `_anchored` matcher
anchored one needle for the wrong reason** (fixed). **AND THE COORDINATOR'S MID-TASK CORRECTION — the
three cells whose expectation was wrong — IS R4 ABOVE.**

## Exit review

*(to be written by the campaign's exit reviewer — not this lane)*
