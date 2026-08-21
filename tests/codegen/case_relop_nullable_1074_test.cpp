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
// §19.3 / #3298 — the DESTRUCTURING-SLOT path, which sema's arm-level
// nullable-scrutinee gate does NOT reach.
//
// §19.3 makes an ordering pattern over a nullable scrutinee a compile error and
// #3298 gates that at both `case` sites, but the gate walks ARM patterns (and
// alternative-list members) only: it does not resolve a destructuring pattern's
// per-slot declared types, so `case r of R(1..5, 1) => …` where `R.X: int?` is
// still admitted and still lowers through the nullable range/relational
// handlers in patternMatches.
//
// These rows are the ratchet on that. Both slot call sites lower a slot with
// `if (subMatch) matched = AND(matched, subMatch);`, so if those handlers ever
// return nullptr the slot's test is SILENTLY SKIPPED and the arm matches
// unconditionally — a wrong answer, not a crash. A draft of #3298 deleted the
// handlers as "dead code after the sema gate" and this exact program returned
// 7 instead of 3. Whatever the eventual §19.3 slot-level ruling is (reject at
// sema, or keep this lowering), it must not be "the slot check disappears".
// ---------------------------------------------------------------------------

namespace {
// `case r of R(<slotPat>, 1) => 7, _ => 3` over `R(X: int?, Y: int)`.
int slotProg(const std::string& xInit, const std::string& slotPat) {
    return run(
        std::string("module M; record R(X: int?, Y: int); "
                    "class C public static method Main(): int "
                    "  var r := new R(") + xInit + ", 1); "
        "  return (case r of R(" + slotPat + ", 1) => 7, _ => 3); "
        "end Main; end C;");
}
}  // namespace

BEZEL_TEST(case_relop_nullable_1074, nullable_slot_range_out_of_range_3298) {
    BEZEL_EXPECT_EQ(slotProg("99", "1..5"), 3);   // 99 not in 1..5
}

BEZEL_TEST(case_relop_nullable_1074, nullable_slot_range_in_range_3298) {
    BEZEL_EXPECT_EQ(slotProg("3", "1..5"), 7);    // 3 in 1..5
}

BEZEL_TEST(case_relop_nullable_1074, nullable_slot_range_null_3298) {
    BEZEL_EXPECT_EQ(slotProg("null", "1..5"), 3); // absent never matches
}

BEZEL_TEST(case_relop_nullable_1074, nullable_slot_relational_3298) {
    BEZEL_EXPECT_EQ(slotProg("3", "> 50"), 3);    // 3 is not > 50
    BEZEL_EXPECT_EQ(slotProg("99", "> 50"), 7);   // 99 is
    BEZEL_EXPECT_EQ(slotProg("null", "> 50"), 3); // absent never matches
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
