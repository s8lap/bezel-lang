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

// §19.3 / §16.6 / #3298 (v0.267): a relational pattern over a nullable
// scrutinee is rejected at sema. runAot returns ran=false when bezelc
// emits a diagnostic; that's the assertion below.
bool runs(const std::string& prog) {
    return runAot(prog).ran;
}
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
std::string caseProg(const std::string& init, const std::string& arm) {
    return std::string("module M; class C public static method Main(): int "
                       "  var n: int? := ") + init + "; "
           "  var r := (case n of " + arm + ", _ => 2); "
           "  return r; end Main; end C;";
}
}  // namespace

// §19.3 / v0.267 / #3298: a present value with a relational pattern is no
// longer admitted — sema rejects the form. Inverted from #1074's
// acceptance that pinned `nv.relop.match`'s silent-never-match.
BEZEL_TEST(case_relop_nullable_1074, present_value_matches_gt_rejected_v0_267) {
    BEZEL_EXPECT_FALSE(runs(caseProg("5", "> 3 => 1")));
}

// §19.3 / v0.267 / #3298: a null scrutinee with a relational pattern is no
// longer silently handled — sema rejects.
BEZEL_TEST(case_relop_nullable_1074, null_scrutinee_falls_through_rejected_v0_267) {
    BEZEL_EXPECT_FALSE(runs(caseProg("null", "> 3 => 1")));
}

// Boundary on `>`: still rejected (semantically meaningless once the
// pattern's scrutinee precondition is gone).
BEZEL_TEST(case_relop_nullable_1074, present_value_below_boundary_gt_rejected_v0_267) {
    BEZEL_EXPECT_FALSE(runs(caseProg("3", "> 3 => 1")));
}

// `<=` at boundary — same gate.
BEZEL_TEST(case_relop_nullable_1074, present_value_matches_le_boundary_rejected_v0_267) {
    BEZEL_EXPECT_FALSE(runs(caseProg("3", "<= 3 => 8")));
}

// `<` and `>=`: both spellings rejected.
BEZEL_TEST(case_relop_nullable_1074, lt_and_ge_forms_rejected_v0_267) {
    BEZEL_EXPECT_FALSE(runs(caseProg("2", "< 5 => 4")));
    BEZEL_EXPECT_FALSE(runs(caseProg("7", ">= 7 => 6")));
}

// A relational arm first, then a constant arm on the same nullable
// scrutinee — the relational arm is the one that triggers the new gate.
BEZEL_TEST(case_relop_nullable_1074, relational_then_constant_arm_rejected_v0_267) {
    BEZEL_EXPECT_FALSE(runs(
        "module M; class C public static method Main(): int "
        "  var n: int? := 10; "
        "  var r := (case n of > 100 => 1, 10 => 5, _ => 2); "
        "  return r; end Main; end C;"));
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
// `*_still_admitted_3898`/`_3899` tests) — a residual NESTED-slot gap remains
// open as issue 3938, also documented and tested below.
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
// so it can no longer reach these handlers at all — but the NESTED shapes
// #3898 does NOT close (issue 3938, `*_still_admitted_3899` rows below,
// unchanged from before #3898) remain admitted and DO reach them: MEASURED
// here, independently re-verifying #3899's own acceptance criterion, that
// stubbing both `nv.range.match` and `nv.relop.match` to `return nullptr;`
// flips `generic_record_slot_nested_pattern_no_match_still_admitted_3899`
// from 3 to 7 — `if (subMatch) matched = AND(matched, subMatch);` skips a
// nullptr'd slot test and the arm matches unconditionally, exactly the
// "→ AND skipped → always matched" hazard above (the sibling `_3899` test,
// whose correct value already IS 7, is unchanged by the stub — matching a
// true outcome vacuously is indistinguishable from matching it for real, so
// only the no-match row demonstrates the defect). Restored, both pass again.
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
bool rejectedAsNullableOrderingPattern(const std::string& prog) {
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
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPattern(
        "module M; record R(X: int?, Y: int); "
        "class C public static method Main(): int "
        "  var r := new R(99, 1); "
        "  return (case r of R(1..5, 1) => 7, _ => 3); "
        "end Main; end C;"));
}

// §19.3 (v0.294, #3692): the RELATIONAL slot sub-pattern form, same field.
BEZEL_TEST(case_relop_nullable_1074, nullable_slot_relational_rejected_3692) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPattern(
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
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPattern(
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
// ⚠ NOT closed by #3898: a generic-record slot NESTED one level down inside
// another destructuring pattern (`Outer(Box(1..5, 1), 9)`) or inside a
// choice-case payload slot (`Val(Box(1..5, 1))`) — `bindNestedDestructuringNames`
// resolves a nested pattern's record purely by NAME, with no substitution
// context of any kind, so these remain wrongly ADMITTED. Filed as issue 3938
// with the two repros below kept as explicit still-admitted controls
// (`generic_record_slot_nested_pattern_3899` / `_in_choice_payload_3899` and
// their `_no_match` siblings) rather than silently dropped.
// ---------------------------------------------------------------------------

// The issue's own repro, RANGE form: `Box<T>(X: T, Y: int)` instantiated
// `Box<int?>`, matched `Box(1..5, 1)`. Before #3898 this compiled and
// returned 3/7/3 depending on X's value (D5's declined reading, MEASURED
// above at commit 85c633a); it now rejects unconditionally, because the
// rejection is on the PATTERN'S KIND, not on a runtime value — mirroring
// `nullable_slot_range_rejected_3692`'s non-generic form above.
BEZEL_TEST(case_relop_nullable_1074, generic_record_slot_range_pattern_rejected_3898) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPattern(
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var b := new Box<int?>(99, 1); "
        "  return (case b of Box(1..5, 1) => 7, _ => 3); "
        "end Main; end C;"));
}

// RELATIONAL form, same generic-field shape.
BEZEL_TEST(case_relop_nullable_1074, generic_record_slot_relational_pattern_rejected_3898) {
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPattern(
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
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPattern(
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
    BEZEL_EXPECT_TRUE(rejectedAsNullableOrderingPattern(
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

// ⚠ Residual gap (issue 3938, NOT closed by #3898): a generic record slot
// NESTED inside another destructuring pattern (`Outer<T>(B: Box<T>, Z:
// int)`) — `bindNestedDestructuringNames` has no substitution context, so
// `Box`'s own bare `T` is still resolved unsubstituted and this program stays
// (wrongly, per §19.3) ADMITTED, computing the D5 value rather than
// rejecting. Kept as an explicit still-admitted control, not silently
// dropped, exactly as #3692 pinned this file's top-level case before #3898.
BEZEL_TEST(case_relop_nullable_1074, generic_record_slot_nested_pattern_still_admitted_3899) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(X: T, Y: int); "
        "record Outer<T>(B: Box<T>, Z: int); "
        "class C public static method Main(): int "
        "  var o := new Outer<int?>(new Box<int?>(3, 1), 9); "
        "  return (case o of Outer(Box(1..5, 1), 9) => 7, _ => 3); "
        "end Main; end C;"), 7);
}
BEZEL_TEST(case_relop_nullable_1074, generic_record_slot_nested_pattern_no_match_still_admitted_3899) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(X: T, Y: int); "
        "record Outer<T>(B: Box<T>, Z: int); "
        "class C public static method Main(): int "
        "  var o := new Outer<int?>(new Box<int?>(99, 1), 9); "
        "  return (case o of Outer(Box(1..5, 1), 9) => 7, _ => 3); "
        "end Main; end C;"), 3);
}

// ⚠ Residual gap (issue 3938): the identical nested shape via a CHOICE-case
// payload — the choice-scrutinee DestructuringPattern branch (fix #3902)
// resolves the payload's OWN type via `common::substitutedChoicePayloadType`
// (or, at sema, `substituteGenericFieldType` since #3898), but the NESTED
// `Box(1..5, 1)` sub-pattern still goes through the same
// substitution-blind `bindNestedDestructuringNames`.
BEZEL_TEST(case_relop_nullable_1074, generic_record_slot_in_choice_payload_still_admitted_3899) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(X: T, Y: int); "
        "choice Wrap<T> Val(B: Box<T>), None; end Wrap; "
        "class C public static method Main(): int "
        "  var w := Wrap<int?>.Val(new Box<int?>(3, 1)); "
        "  return (case w of Val(Box(1..5, 1)) => 7, _ => 3); "
        "end Main; end C;"), 7);
}
BEZEL_TEST(case_relop_nullable_1074, generic_record_slot_in_choice_payload_no_match_still_admitted_3899) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(X: T, Y: int); "
        "choice Wrap<T> Val(B: Box<T>), None; end Wrap; "
        "class C public static method Main(): int "
        "  var w := Wrap<int?>.Val(new Box<int?>(99, 1)); "
        "  return (case w of Val(Box(1..5, 1)) => 7, _ => 3); "
        "end Main; end C;"), 3);
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
