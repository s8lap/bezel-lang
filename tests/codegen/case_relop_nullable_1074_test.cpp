// Regression tests for #1074 + #3298 (§19.3 / §16.6): a RELATIONAL pattern
// (`> 3`, `<= n`, …) in a `case` over a NULLABLE value-type scrutinee.
//
// v0.267 (proposal 0114 / #3298) deliberately OVERRIDES D5's pre-v0.267
// admission: a relational or range pattern over a nullable value-type
// scrutinee is now a compile error (§19.3 / §16.6 / §7.11). The pre-v0.267
// behavior — `case n of > 3 …` silently never matched a null scrutinee via
// `nv.relop.match` (LlvmBackend_Exprs2.cpp:8990–9030) — violated §16.6's
// standing claim that range patterns "agree exactly with `in`".
//
// ⚠ That claim held on NEITHER side before #3298, contrary to what the issue
// and proposal 0114 both assert. MEASURED on the pre-fix compiler: `case n of
// > 3 …` compiled and silently never matched, and `n in 1..5` over an `int?`
// did NOT reject — it reached codegen and died in IR verification (`icmp sge
// {i8,i32}, i32`). #3298 delivers the parity by gating BOTH: §19.3's pattern
// rule (here) and §17.5's range-receiver rule for `in`.
//
// The #1074 RELATIONAL-PATTERN blocks (originally acceptance tests pinning
// the silent-never-match reading) are inverted to rejection tests below.
// The adjacent `…_1113` blocks test §8.4 equality in EXPRESSION position
// and stay acceptance — equality is unaffected by v0.267 (proposal D2).
//
// runJit returns the raw exit code (0..255).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// §19.3 / §16.6 / #3298 (v0.267; v0.310, #5143 ruling): a relational pattern
// over an UN-narrowed nullable scrutinee is rejected at sema. runAot returns
// ran=false when bezelc emits a diagnostic; the assertion below confirms the
// specific §19.3/§16.6/§7.11 needle.
bool runs(const std::string& prog) {
    return runAot(prog).ran;
}
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// §19.3 (v0.310, #5143 ruling): the caseProg helper takes an un-narrowed
// scrutinee — a parameter `n: int?` is not in `nullNarrowed_`, so the gate
// fires. The original `var n: int? := <init>` form narrowed `n` at the
// assignment (narrowOnAssignment's `valueProvesNonNull(5, int?) = true`)
// which made the gate vacuous after #5173; the parameter form restores the
// un-narrowed condition #1074 originally pinned.
std::string caseProg(const std::string& arm) {
    return std::string("module M; class C public static method Main(n: int?): int "
                       "  var r := (case n of ") + arm + ", _ => 2); "
           "  return r; end Main; end C;";
}
// Assert that `prog` is rejected at sema with the §19.3/§16.6/§7.11
// relational-pattern needle — strict, not just `!r.ran`.
bool rejectedAsNullableOrderingPattern(const std::string& prog) {
    auto r = bezel::test::runAot(prog);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("relational pattern requires a non-nullable scrutinee") ==
                std::string::npos)
            continue;
        if (m.find("19.3") == std::string::npos) continue;
        if (m.find("16.6") == std::string::npos) continue;
        if (m.find("7.11") == std::string::npos) continue;
        return true;
    }
    return false;
}
}  // namespace

// §19.3 / v0.267 / #3298; v0.310 / #5143 ruling: a present value with a
// relational pattern over an UN-narrowed nullable scrutinee still rejects.
// Inverted from #1074's acceptance that pinned `nv.relop.match`'s
// silent-never-match.
BEZEL_TEST(case_relop_nullable_1074, present_value_matches_gt_rejected_v0_267) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPattern(caseProg("> 3 => 1")));
}

// §19.3 / v0.267 / #3298; v0.310 / #5143 ruling: an un-narrowed nullable
// scrutinee with a relational pattern still rejects. (The narrowed variant
// is covered by `_admitted_5173` below.)
BEZEL_TEST(case_relop_nullable_1074, null_scrutinee_falls_through_rejected_v0_267) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPattern(caseProg("> 3 => 1")));
}

// Boundary on `>`: still rejected (semantically meaningless once the
// pattern's scrutinee precondition is gone).
BEZEL_TEST(case_relop_nullable_1074, present_value_below_boundary_gt_rejected_v0_267) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPattern(caseProg("> 3 => 1")));
}

// `<=` at boundary — same gate.
BEZEL_TEST(case_relop_nullable_1074, present_value_matches_le_boundary_rejected_v0_267) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPattern(caseProg("<= 3 => 8")));
}

// `<` and `>=`: both spellings rejected.
BEZEL_TEST(case_relop_nullable_1074, lt_and_ge_forms_rejected_v0_267) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPattern(caseProg("< 5 => 4")));
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPattern(caseProg(">= 7 => 6")));
}

// A relational arm first, then a constant arm on the same nullable
// scrutinee — the relational arm is the one that triggers the new gate.
BEZEL_TEST(case_relop_nullable_1074, relational_then_constant_arm_rejected_v0_267) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPattern(
        "module M; class C public static method Main(n: int?): int "
        "  var r := (case n of > 100 => 1, 10 => 5, _ => 2); "
        "  return r; end Main; end C;"));
}

// ---------------------------------------------------------------------------
// fix #5173 / §19.3 (v0.310, #5143 ruling): narrowed non-null reads of a
// nullable scrutinee are ADMITTED. The narrowed variant runs through
// patternMatches's `isNullableVal` branch, which extracts the inner `i32`
// payload and compares against the pattern bound. Each accept row asserts
// the SELECTED arm's value on stdout.
// ---------------------------------------------------------------------------

// `n` narrowed by `if n <> null`; `5` matches `> 3` and selects 1.
//
// `Main(n: int?)` is NOT a §33.2 entry-point signature (only `Main()` or
// `Main(args: Array<string>)` are); Helper.Get() returns `int?` so
// `valueProvesNonNull` returns false and `n` is NOT narrowed at the
// declaration — the `if n <> null then` block narrows `n` for the case
// form (§7.4; §19.1). The repeated "class helper ... end Helper" prelude is
// the only way to get a non-literal `int?` initial value at a no-arg Main.
BEZEL_TEST(case_relop_nullable_1074,
           present_value_matches_gt_narrowed_admitted_5173) {
    auto r = runAot(
        "module M; class Helper public static method Get(): int? "
        "  return 5; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: int? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of > 3 => 1, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// `n` narrowed, `3` does NOT match `> 3` and selects the `_ => 2` arm.
BEZEL_TEST(case_relop_nullable_1074,
           present_value_below_boundary_gt_narrowed_admitted_5173) {
    auto r = runAot(
        "module M; class Helper public static method Get(): int? "
        "  return 3; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: int? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of > 3 => 1, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// `n` narrowed, `3` matches `<= 3` (boundary inclusive).
BEZEL_TEST(case_relop_nullable_1074,
           present_value_matches_le_boundary_narrowed_admitted_5173) {
    auto r = runAot(
        "module M; class Helper public static method Get(): int? "
        "  return 3; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: int? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of <= 3 => 8, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);
}

// `<` and `>=` spellings, narrowed — `2` matches `< 5`; `7` matches `>= 7`.
BEZEL_TEST(case_relop_nullable_1074,
           lt_and_ge_forms_narrowed_admitted_5173) {
    auto r = runAot(
        "module M; class Helper public static method Get(): int? "
        "  return 2; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: int? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of < 5 => 4, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
    auto r2 = runAot(
        "module M; class Helper public static method Get(): int? "
        "  return 7; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: int? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of >= 7 => 6, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r2.ran);
    BEZEL_EXPECT_EQ(r2.exitCode, 6);
}

// A relational arm then a constant arm, narrowed — `10` does NOT match
// `> 100`, so the constant arm `10 => 5` selects.
BEZEL_TEST(case_relop_nullable_1074,
           relational_then_constant_arm_narrowed_admitted_5173) {
    auto r = runAot(
        "module M; class Helper public static method Get(): int? "
        "  return 10; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: int? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of > 100 => 1, 10 => 5, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ---------------------------------------------------------------------------
// §19.3 / #3298 / #3692 — the DESTRUCTURING-SLOT path.
//
// §19.3 makes an ordering pattern over a nullable scrutinee a compile error.
// #3298 gated that at the ARM level (and alternative-list members) but did NOT
// resolve a destructuring pattern's per-slot declared types, so
// `case r of R(1..5, 1) => …` where `R.X: int?` was still admitted and lowered
// through the nullable range/relational handlers in patternMatches, silently
// taking proposal 0114's declined D5 reading ("matches every present value,
// never null") instead of being rejected — MEASURED pre-#3692: the four
// `slotProg` calls (the shape now inlined into the `_rejected_3692` rows
// below) returned 3/7/3/(3,7,3).
//
// #3692 closes that gap for a slot whose declared type sema can see directly
// — a NON-GENERIC record field (below) or a CHOICE payload field, generic or
// not (tests/sema/lifted_operators_rejected_3298_test.cpp's
// `case_range_pattern_in_choice_payload_slot_over_nullable_field_rejected_3692`
// — the choice path already substitutes a generic instantiation via
// `ExprTC::substituteGenericCaseFieldType`) — by feeding the arm-level walk's
// shared single-pattern helper (`checkOrderingPatternOverNullableType`) the
// slot's OWN resolved type at all six existing slot-type-resolution call
// sites (CaseExpr x CaseStmt, record x choice, top-level x nested), rather
// than adding a further copy of slot-type resolution.
//
// ⚠ HISTORICAL (superseded below): at the revision this comment was
// written, it did NOT close the gap for a GENERIC RECORD's field typed at a
// bare type parameter (`record Box<T>(X: T, Y: int)`) — the record
// positional-slot loops resolved `fieldTy` from the record's declared
// (unsubstituted) field type, with no record analogue of the choice path's
// substitution helper. That was filed as issue 3898 and is CLOSED for the
// TOP-LEVEL (non-nested) case as of the fix landing in this file (see the
// "fix #3898" comment block below, and its `*_rejected_3898` /
// `*_still_admitted_3898`/`_3899` tests) — the residual NESTED-slot gap
// that #3898 left open as issue 3938 is itself CLOSED by fix #3938
// (this file's `*_rejected_3938` rows at the bottom): the four caller
// loops now hand `bindNestedDestructuringNames` a worklist pre-populated
// with each pending nested pattern's own substituted scrutTy, and the walk
// re-substitutes the next level's slot types against that scrutTy.
//
// That then-residual, sema-admitted path turned out to ALSO be independently
// broken in codegen, for an unrelated reason discovered while checking
// whether it still reaches the SLOT-PATH-ONLY handlers correctly: filed as
// issue 3899. ⚠ The issue's OWN filed root cause ("armSubst installed too
// late, after patternMatches has already run") does NOT hold, per commit
// 85c633a's own investigation: `patternMatches`'s record-scrutinee
// DestructuringPattern branch installs its OWN `RecordSubstGuard`
// (`destrSubst`) before its slot loops — `armSubst`/the arm-body guard is a
// DIFFERENT one (fix #1254) and is not implicated here. (That stale framing
// still lingers as a comment at LlvmBackend_Exprs2.cpp:10653-10709, added by
// #3692 before 85c633a's investigation refuted it — tracked separately as
// issue 3946; not fixed here.) The ACTUAL bug: the three slot-type-resolution
// call sites in that branch read `fieldTy` via the bare `recordMemberType`
// (the immediate, unsubstituted declared type) instead of
// `recordMemberTypeResolved` (which consults the active substitution) —
// CLOSED at commit 85c633a by swapping in `recordMemberTypeResolved` at all
// three call sites — see the "fix #3899" comment block below.
//
// Both slot call sites in patternMatches's DestructuringPattern branch lower
// a slot with `if (subMatch) matched = AND(matched, subMatch);`, so if the
// SLOT-PATH-ONLY nv.relop.match / nv.range.match handlers ever return
// nullptr the slot's test is SILENTLY SKIPPED and the arm matches
// unconditionally — a wrong answer, not a crash. A draft of #3298 deleted
// the handlers as "dead code after the sema gate" and this exact program
// returned 7 instead of 3. They are KEPT here too, unconditionally — see the
// LlvmBackend_Exprs2.cpp SLOT-PATH ONLY comment for why they still matter.
// Since #3898 landed, a TOP-LEVEL generic-record nullable slot with an
// ordering pattern is REJECTED at sema (the `*_rejected_3898` rows below),
// so it can no longer reach these handlers at all. Since #3938 landed, the
// NESTED shapes #3898 left open (`*_rejected_3938` rows at the bottom of
// this block) are ALSO rejected at sema — so they, too, can no longer
// reach the slot-path-only handlers. The SLOT-PATH-ONLY handlers' own
// acceptance criterion (they correctly lower an admitted null-slot match)
// is exercised today only by #3692's NON-GENERIC nested case
// (`nullable_nested_slot_range_rejected_3692` — admitted pre-#3692, the
// criterion that let the codegen defect be discovered in the first
// place; the stub flipped its value exactly as the comment block above
// predicts). Restored, that row rejects at sema per #3692, exactly as
// before #3938.
//
// The CHOICE side is closed at SEMA by #3692's own fix (a nullable choice
// payload with an ordering pattern rejects, non-generic and generic alike).
// Searching that side surfaced a further, unrelated bug (issue 3902): choice
// payload slots are never checked at codegen for ANY refutable pattern
// (constant, range, relational — nullability irrelevant) — real and severe,
// but orthogonal to this file's gap.
// ---------------------------------------------------------------------------

namespace {
// §19.3 / #3692: a destructuring slot's ordering sub-pattern over a NULLABLE
// field is now a compile error. runAot's REJECT diagnostic carries no real
// source location (it is packed into one synthetic {0,0} Diagnostic), so this
// asserts message TEXT — the same §19.3/§16.6/§7.11 wording the arm-level gate
// already uses (checkOrderingPatternOverNullableType is the single shared
// walk), not merely `!r.ran` (which is also true on a runtime signal death).
// Renamed from rejectedAsNullableOrderingPattern to avoid colliding with the
// file-level helper above (different strictness: this matches any
// "pattern requires a non-nullable scrutinee", not just "relational").
bool rejectedAsNullableOrderingPatternInSlot(const std::string& prog) {
    auto r = bezel::test::runAot(prog);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("pattern requires a non-nullable scrutinee") ==
                std::string::npos)
            continue;
        if (m.find("19.3") == std::string::npos) continue;
        if (m.find("16.6") == std::string::npos) continue;
        if (m.find("7.11") == std::string::npos) continue;
        return true;
    }
    return false;
}
}  // namespace

// §19.3 (v0.294, #3692): a record-destructuring slot's RANGE sub-pattern over
// a nullable field (`R.X: int?`) is a compile error — the issue's verbatim
// repro. Before #3692 this compiled and returned 3/7/3 via the D5 reading
// (measured above); it now rejects for every scrutinee value, present or
// null, because the rejection is on the PATTERN'S KIND, not on a runtime
// value.
BEZEL_TEST(case_relop_nullable_1074, nullable_slot_range_rejected_3692) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPatternInSlot(
        "module M; record R(X: int?, Y: int); "
        "class C public static method Main(): int "
        "  var r := new R(99, 1); "
        "  return (case r of R(1..5, 1) => 7, _ => 3); "
        "end Main; end C;"));
}

// §19.3 (v0.294, #3692): the RELATIONAL slot sub-pattern form, same field.
BEZEL_TEST(case_relop_nullable_1074, nullable_slot_relational_rejected_3692) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPatternInSlot(
        "module M; record R(X: int?, Y: int); "
        "class C public static method Main(): int "
        "  var r := new R(99, 1); "
        "  return (case r of R(> 50, 1) => 7, _ => 3); "
        "end Main; end C;"));
}

// §19.3 (v0.294, #3692): the rejection also reaches a slot nested TWO levels
// deep (`Outer(Inner(1..5), b)`), which is a SEPARATE call site
// (the nested-destructuring worklist) from the top-level slot loop above.
BEZEL_TEST(case_relop_nullable_1074, nullable_nested_slot_range_rejected_3692) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPatternInSlot(
        "module M; record Inner(V: int?); record Outer(I: Inner, B: int); "
        "class C public static method Main(): int "
        "  var o := new Outer(new Inner(99), 1); "
        "  return (case o of Outer(Inner(1..5), 1) => 7, _ => 3); "
        "end Main; end C;"));
}

// §19.3 (v0.294, #3692): a CONSTANT slot over the same nullable field stays
// ADMITTED — "Constant patterns over a nullable scrutinee are unaffected: a
// constant pattern is the equality test of §8.4, which is total on `null`."
// This is the LOUD->QUIET guard: the widened rejection must stay exactly as
// wide as §19.3 says, not swallow the constant-slot case too.
BEZEL_TEST(case_relop_nullable_1074, nullable_slot_constant_still_admitted_3692) {
    BEZEL_EXPECT_EQ(run(
        "module M; record R(X: int?, Y: int); "
        "class C public static method Main(): int "
        "  var r := new R(5, 1); "
        "  return (case r of R(5, 1) => 7, _ => 3); "
        "end Main; end C;"), 7);
}

// ---------------------------------------------------------------------------
// fix #3899 (§14.1 / §19.3, v0.294): CLOSED codegen's half — once a generic
// record's slot pattern over a nullable bare-type-parameter field IS admitted
// (below), it correctly reaches the SLOT-PATH-ONLY nullable-value handlers
// (`nv.range.match` / `nv.relop.match`, `LlvmBackend_Exprs2.cpp`) instead of
// failing IR verification (`icmp {i8,i32}, i32`).
//
// fix #3898 (§14.1 / §19.3, v0.294): CLOSED sema's half for the TOP-LEVEL
// (non-nested) case — the case this file's tests exercised as "still
// admitted" above. `ExprTC::substituteGenericFieldType` (generalized from the
// choice-only `substituteGenericCaseFieldType` #2219 introduced) is now
// called at the record-scrutinee slot-type-resolution sites too (CaseExpr and
// CaseStmt), exactly as the choice-payload site already did, so
// `Box<int?>`'s slot 0 resolves to the SUBSTITUTED `int?` before §19.3's
// slot-level ordering-pattern gate (#3692) runs — and that gate now correctly
// REJECTS `Box(1..5, 1)` over `Box<int?>`, symmetric with the non-generic
// `R(1..5, 1)` over `R(X: int?, ...)` #3692 already rejected.
//
// This FLIPS every test below that exercises a TOP-LEVEL generic-record slot
// with an ordering (range/relational) sub-pattern over its bare-type-param
// field from a VALUE assertion to a REJECTION assertion — the program that
// used to compile and compute 3/7/3 (D5's declined reading) now correctly
// fails to compile, per the "fix must update existing tests" rule. Measured
// as part of #3898's FAIL-WITHOUT/PASS-WITH: with `substituteGenericFieldType`
// reverted to Choice-only, every `*_rejected_3898` row below FAILS (the
// program still compiles and runs); restored, all pass.
//
// ⚠ NOT closed by #3898 (was), closed by #3938: a generic-record slot
// NESTED one level down inside another destructuring pattern
// (`Outer(Box(1..5, 1), 9)`) or inside a choice-case payload slot
// (`Val(Box(1..5, 1))`) — `bindNestedDestructuringNames` resolved a nested
// pattern's record purely by NAME, with no substitution context of any
// kind, so before #3938 these remained wrongly ADMITTED. Filed as issue
// 3938 with the two repros kept as explicit still-admitted controls
// (`generic_record_slot_nested_pattern_3899` / `_in_choice_payload_3899`
// and their `_no_match` siblings, since renamed `_rejected_3938`) rather
// than silently dropped.
// ---------------------------------------------------------------------------

// The issue's own repro, RANGE form: `Box<T>(X: T, Y: int)` instantiated
// `Box<int?>`, matched `Box(1..5, 1)`. Before #3898 this compiled and
// returned 3/7/3 depending on X's value (D5's declined reading, MEASURED
// above at commit 85c633a); it now rejects unconditionally, because the
// rejection is on the PATTERN'S KIND, not on a runtime value — mirroring
// `nullable_slot_range_rejected_3692`'s non-generic form above.
BEZEL_TEST(case_relop_nullable_1074, generic_record_slot_range_pattern_rejected_3898) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPatternInSlot(
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var b := new Box<int?>(99, 1); "
        "  return (case b of Box(1..5, 1) => 7, _ => 3); "
        "end Main; end C;"));
}

// RELATIONAL form, same generic-field shape.
BEZEL_TEST(case_relop_nullable_1074, generic_record_slot_relational_pattern_rejected_3898) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPatternInSlot(
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var b := new Box<int?>(10, 1); "
        "  return (case b of Box(> 5, 1) => 7, _ => 3); "
        "end Main; end C;"));
}

// Statement-position `case` (LlvmBackend_Stmts.cpp's `lowerCaseStmt` / the
// CaseStmt sema handler) — a SEPARATE call site from the expression form
// above; the fix's CaseStmt-side `substituteGenericFieldType` call must reach
// it too.
BEZEL_TEST(case_relop_nullable_1074, generic_record_slot_range_pattern_stmt_form_rejected_3898) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPatternInSlot(
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var b := new Box<int?>(3, 1); "
        "  case b of "
        "    Box(1..5, 1) => do return 7; end, "
        "    _ => do return 3; end "
        "  end; "
        "end Main; end C;"));
}

// A SECOND generic parameter position (`Pair<A,B>`, ordering on B) — the
// substitution walk must match the RIGHT parameter by name, not just "the
// first one" or "any one".
BEZEL_TEST(case_relop_nullable_1074, generic_record_second_type_param_slot_rejected_3898) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPatternInSlot(
        "module M; record Pair<A, B>(X: A, Y: B); "
        "class C public static method Main(): int "
        "  var p := new Pair<int, int?>(1, 3); "
        "  return (case p of Pair(1, 1..5) => 7, _ => 3); "
        "end Main; end C;"));
}

// POSITIVE CONTROL 1 (§19.3 stays exactly as wide as it says): the SAME
// generic-record slot shape instantiated at a NON-nullable type argument
// (`Box<int>`, not `Box<int?>`) must remain ADMITTED and compute the correct
// D5-style match value — the fix must reject only where the SUBSTITUTED type
// is actually nullable, not every generic record slot.
BEZEL_TEST(case_relop_nullable_1074, generic_record_slot_non_nullable_instantiation_still_admitted_3898) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var b := new Box<int>(3, 1); "
        "  return (case b of Box(1..5, 1) => 7, _ => 3); "
        "end Main; end C;"), 7);
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var b := new Box<int>(99, 1); "
        "  return (case b of Box(1..5, 1) => 7, _ => 3); "
        "end Main; end C;"), 3);
}

// POSITIVE CONTROL 2: two DIFFERENT instantiations of the same generic
// record matched in sequence, in one function, via CONSTANT slots (not
// ordering patterns, so neither triggers the new rejection) — exercises
// `RecordSubstGuard`'s save/restore end to end (sema's `substituteGenericFieldType`
// AND codegen's per-arm `destrSubst`) and proves the sema fix does not
// perturb the non-ordering destructuring path at all.
BEZEL_TEST(case_relop_nullable_1074, generic_record_slot_two_instantiations_value_admitted_3898) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var a := new Box<int?>(3, 1); "
        "  var b := new Box<string?>(\"hi\", 1); "
        "  var r1 := (case a of Box(3, 1) => 7, _ => 3); "
        "  var r2 := (case b of Box(\"hi\", 1) => 11, _ => 13); "
        "  return r1 + r2; "
        "end Main; end C;"), 18);
}

// ⚠ Residual gap (issue 3938, closed by fix #3938): a generic record slot
// NESTED inside another destructuring pattern (`Outer<T>(B: Box<T>, Z:
// int)`) — `bindNestedDestructuringNames` now threads each pending nested
// pattern's already-substituted scrutinee TypeRef through its worklist
// (the parent slot's substituted type, computed by the caller loop via
// `substituteGenericFieldType`), so `Box`'s own bare `T` is now substituted
// against the parent's instantiation and this program correctly REJECTS
// the range pattern at the substituted nullable type — symmetric with the
// top-level `generic_record_slot_range_pattern_rejected_3898` above and
// with the non-generic `nullable_nested_slot_range_rejected_3692` two
// sections above. MEASURED in #3938's FAIL-WITHOUT/PASS-WITH: with the
// four caller loops reverted to passing the outer subPatterns (no
// substituted worklist) and the walk reverted to its name-only lookup,
// every `*_rejected_3938` row below FAILS (the program still compiles
// and runs); restored, all reject on the §19.3/§16.6/§7.11 wording.
BEZEL_TEST(case_relop_nullable_1074, generic_record_slot_nested_pattern_rejected_3938) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPatternInSlot(
        "module M; record Box<T>(X: T, Y: int); "
        "record Outer<T>(B: Box<T>, Z: int); "
        "class C public static method Main(): int "
        "  var o := new Outer<int?>(new Box<int?>(3, 1), 9); "
        "  return (case o of Outer(Box(1..5, 1), 9) => 7, _ => 3); "
        "end Main; end C;"));
}
// Same nested shape with a value that does NOT match — the rejection is on
// the pattern's KIND, not on the runtime value, so this rows confirms the
// §19.3 rejection covers both arms' sub-patterns symmetrically (no
// accidental `r1 = 7`/`r2 = 3` half-rejection).
BEZEL_TEST(case_relop_nullable_1074, generic_record_slot_nested_pattern_no_match_rejected_3938) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPatternInSlot(
        "module M; record Box<T>(X: T, Y: int); "
        "record Outer<T>(B: Box<T>, Z: int); "
        "class C public static method Main(): int "
        "  var o := new Outer<int?>(new Box<int?>(99, 1), 9); "
        "  return (case o of Outer(Box(1..5, 1), 9) => 7, _ => 3); "
        "end Main; end C;"));
}

// ⚠ Residual gap (issue 3938, closed by fix #3938): the identical nested
// shape via a CHOICE-case payload. The choice-scrutinee DestructuringPattern
// branch's caller loop now hands each nested pattern its own already-
// substituted `fieldTy` (the substituted payload type, computed via
// `substituteGenericFieldType(fieldTy, scrutTy)`, #2219's pre-existing
// choice-side substitution), so `Box`'s bare `T` is substituted against the
// choice's instantiation too — `Box(1..5, 1)` over `Wrap<int?>.Val(...)`
// now correctly REJECTS, exactly as the top-level choice path
// `generic_record_slot_in_choice_payload_range_pattern_rejected_3938`
// below rejects its top-level sibling.
BEZEL_TEST(case_relop_nullable_1074, generic_record_slot_in_choice_payload_rejected_3938) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPatternInSlot(
        "module M; record Box<T>(X: T, Y: int); "
        "choice Wrap<T> Val(B: Box<T>), None; end Wrap; "
        "class C public static method Main(): int "
        "  var w := Wrap<int?>.Val(new Box<int?>(3, 1)); "
        "  return (case w of Val(Box(1..5, 1)) => 7, _ => 3); "
        "end Main; end C;"));
}
BEZEL_TEST(case_relop_nullable_1074, generic_record_slot_in_choice_payload_no_match_rejected_3938) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPatternInSlot(
        "module M; record Box<T>(X: T, Y: int); "
        "choice Wrap<T> Val(B: Box<T>), None; end Wrap; "
        "class C public static method Main(): int "
        "  var w := Wrap<int?>.Val(new Box<int?>(99, 1)); "
        "  return (case w of Val(Box(1..5, 1)) => 7, _ => 3); "
        "end Main; end C;"));
}

// ---------------------------------------------------------------------------
// #1113 (§7.11 / §8.4): a NARROW nullable (`byte?`/`short?`/`sbyte?`) compared
// to an int LITERAL (which lowers at i32) crashed the compiler — the §8.4
// null-safe equality ICmp'd the i8/i16 payload against the i32 literal (an LLVM
// same-type assert). The fix widens the narrower operand to the wider (each by
// its own signedness), mirroring the non-nullable comparison — NOT truncating
// the literal (which would wrap an out-of-range value into the payload range).
// §8.4 equality in EXPRESSION position is unaffected by v0.267 (proposal
// D2 retains `=`/`<>`); these blocks stay acceptance.
// ---------------------------------------------------------------------------

// byte? = <int-literal>: present value equal (was a compiler crash).
BEZEL_TEST(case_relop_nullable_1074, narrow_nullable_eq_int_literal_1113) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: byte? := 5; return (if a = 5 then 1 else 0); end Main; end C;"), 1);
}

// byte? <> <int-literal>: present value not-equal (was a compiler crash).
BEZEL_TEST(case_relop_nullable_1074, narrow_nullable_ne_int_literal_1113) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: byte? := 5; return (if a <> 6 then 1 else 0); end Main; end C;"), 1);
}

// Out-of-range literal: `byte?(5) = 261` — 261 & 0xFF == 5, so a TRUNCATING fix
// would wrongly report equal. Widening compares 5 vs 261 → NOT equal.
BEZEL_TEST(case_relop_nullable_1074, narrow_nullable_eq_out_of_range_literal_1113) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: byte? := 5; return (if a = 261 then 1 else 0); end Main; end C;"), 0);
}

// sbyte? signed: the payload sign-extends so a negative literal compares by value.
BEZEL_TEST(case_relop_nullable_1074, narrow_signed_nullable_eq_negative_literal_1113) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: sbyte? := -5; return (if a = -5 then 1 else 0); end Main; end C;"), 1);
}

// short? = <int-literal> with the literal LHS (operand order swapped).
BEZEL_TEST(case_relop_nullable_1074, narrow_nullable_eq_literal_lhs_1113) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: short? := 300; return (if 300 = a then 1 else 0); end Main; end C;"), 1);
}

// null narrow-nullable = literal → not equal (null is never equal to a value).
BEZEL_TEST(case_relop_nullable_1074, null_narrow_nullable_eq_literal_1113) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: byte? := null; return (if a = 5 then 1 else 0); end Main; end C;"), 0);
}
