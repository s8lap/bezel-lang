// Spec-derived sema tests for fix #3298 — §7.11 lifting rejection widened
// to primitive operators (proposal 0114; landed at v0.267, re-derived here
// against v0.286).
//
// Authority: spec §7.11, whose text is BYTE-IDENTICAL at v0.267 and v0.286
// (verified by diffing the section across the two revisions). Verbatim, the
// clauses these tests encode:
//
//   "No operator **lifts** to a nullable type. For a nullable value type
//   `T?`, the operators [§3.5] defines on the underlying type `T` are not
//   defined on `T?`: an operand whose static type is a nullable value type
//   is a **compile error** at every arithmetic, bitwise, shift, and
//   relational operator — on either side, and in the mixed `T?`/`T` pairing
//   alike — with the diagnostic naming the remedies the language uses at
//   every other position where a nullable value meets a non-nullable one: a
//   [§7.4] narrowing check, `??` ([§7.7]), or `!` ([§7.8]). The rule reads
//   the operand's **static** type, so an operand that [§7.4] has narrowed
//   non-null is a non-nullable operand and the ordinary operator applies to
//   it unchanged; no separate rule is needed for that case. Nullable
//   **reference** operands take the same answer at their own types ..."
//
//   "**The rejected operators.** The binary `+`, `-`, `*`, `/`, `mod`, `&`,
//   `|`, `^`, `<<`, `>>`; the unary `+`, `-`, `~`; and the relational `<`,
//   `<=`, `>`, `>=`. Their compound-assignment forms are rejected with them
//   ([§6.6]), as are `++` and `--` ... — `??=` excepted, which is not a
//   read-modify-write ([§6.6])."
//
// The pre-fix implementation lifted every arithmetic / bitwise / shift /
// relational operator over a nullable primitive operand type (the §7.11
// propagation semantics): the result was `T?` for arith/bitwise/shift and
// `bool` (always `false` on null) for relational. Three defects this lands
// the rejection for, each verified by at least one test below:
//
//  (1) THE FAIL-OPEN GUARD (§7.11/§1.1 of proposal 0114). `flags & ADMIN`
//      lifted to `uint?`, and `null <> 0` was `true`, so an authorization
//      test `if (flags & ADMIN) <> 0 then ...` reached the protected branch
//      on missing input. Reproduced by the `fail_open_guard_*` tests.
//
//  (2) THE STICKY ACCUMULATOR (§6.6 / §1.4). `total += p` lifted to
//      `int?` and then to `int?` on every iteration; one null `p` poisoned
//      `total` permanently and every later `+=` preserved it. Reproduced
//      by the `sticky_accumulator_*` tests.
//
//  (3) THE §16.6 PARITY (§19.3 / §16.6 / §17.5). The issue and proposal 0114
//      both asserted `n in 1..5` over an `int?` "was already a compile
//      error". MEASURED ON THE PARENT COMPILER, IT WAS NOT: it reached
//      codegen and died in IR verification (`icmp {i8,i32}, i32`) — a
//      compiler ICE, not a diagnosis — while `case n of 1..5` compiled and
//      silently never matched. So NEITHER side of §16.6's parity claim held,
//      and the parity is delivered here by gating BOTH: §17.5's range-receiver
//      rule (`Range<T>.Contains` takes the non-nullable `T` of §16.3, and
//      membership over a range is an ordering test) and §19.3's pattern rule.
//      §17.5's OTHER half is deliberately untouched: where the selected
//      `Contains` parameter is itself nullable (`Array<int?>`, the span forms)
//      a nullable left operand stays ADMITTED — that test is §8.4 equality,
//      which §7.11 retains (v0.270, #3312.2). Reproduced by the
//      `case_pattern_over_nullable_*` and `in_range_*` tests.
//
// The retained set (per proposal D2/D3): `=`/`<>` (§8.4), `??` (§7.7),
// `?.` (§7.5), `?(args)` (§7.6), and `??=` (a compound form that is
// structurally "not a read-modify-write" per §6.6). Each is asserted in
// the `retained_*` tests. ⚠ The trace case `n ??= 0; n += 1;` (the issue's
// own "must reject only at the `+=`" wording) was this file's signature
// acceptance test at the time it was written; fix #3830 (v0.288, ruling on
// issue 3686) composes with the PRE-EXISTING `??=`-narrows rule (§7.4
// v0.210, #2104) to admit the whole trace — see
// `qq_eq_then_compound_narrowed_admitted_3830` below, which is now the
// test that pins it (and its `qq_eq_with_nullable_rhs_leaves_target_
// unnarrowed` control, which keeps the "unnarrowed target still rejects"
// half alive).

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

// fix #3958 (issue 3958) gated this on parse diagnostics so a program that
// fails to parse never reaches sema here either — before that gate, a
// malformed row's parser-error-recovery-truncated AST could still produce a
// diagnostic this file's helpers matched, masking six grammar-invalid rows
// (five missing-comma `CaseStmt` arms, one stray `end Identifier;` after an
// arrow-bodied method) that "passed" for the wrong reason. All were
// corrected to parse cleanly as part of that fix.
//
// test #3958 / fix #3996 (issue 3996): #3958's own gate checked
// `!r.diagnostics.hasErrors()` AFTER `bezel::test::parseAndCollect`, which
// always runs `collectSymbols` once `sourceFile` is non-null — so a
// collectSymbols-stage error (e.g. a §3.4 reserved-primitive-alias-name
// rejection) would have incorrectly also gated `resolveTypes`/
// `typecheckExpressions` here, unlike the real driver (Driver.cpp ~473-489),
// which evaluates its own gate once, immediately after parsing and BEFORE
// collectSymbols runs, then runs collectSymbols/resolveTypes/
// typecheckExpressions unconditionally inside that one surviving branch
// (issue 3996 found and fixed the identical defect in the shared
// tests/support/sema_support.hpp helpers this file's shape matches).
// Delegates to that shared, correctly-gated helper instead of keeping a
// second, independently-drifting copy of the same gate (CLAUDE.md's
// duplicated-classification-predicate caution) — one authority beats a
// corrected copy.
bezel::test::SemaResult parseSemaAll(std::string_view src) {
    return bezel::test::parseAndTypecheckGated(src);
}

// §7.11 / #3298: the diagnostic family the new gate emits. ALL three
// remedies the spec names (§7.4 narrowing, `??`, `!`) appear in one
// message so the user sees the same shape regardless of which form
// they reach for first; this is the §7.11 wording, not the §6.6 wording
// the compound form inherits.
bool hasNullableOpError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("not defined on a nullable value type") ==
                std::string::npos)
            continue;
        if (d.message.find("7.11") == std::string::npos)
            continue;
        if (d.message.find("narrow first") == std::string::npos)
            continue;
        if (d.message.find("`??`") == std::string::npos)
            continue;
        if (d.message.find("`!`") == std::string::npos)
            continue;   // keep scanning: a LATER diagnostic may be the one
        return true;
    }
    return false;
}

// §6.6 / #3298: the compound form's diagnostic — wider than the §7.11
// gate, names which operand direction is nullable, and points the user
// at the non-nullable compound form.
bool hasNullableCompoundError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("compound assignment") == std::string::npos) continue;
        if (m.find("nullable") == std::string::npos) continue;
        if (m.find("7.11") == std::string::npos) continue;
        if (m.find("6.6") == std::string::npos) continue;
        return true;
    }
    return false;
}

// §7.11 / §19.3 / §16.6 / #3298: the pattern companion's diagnostic.
bool hasNullablePatternError(const bezel::test::SemaResult& r) {
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

// §17.5 / §16.6 / #3298: the range-receiver `in` gate's diagnostic. Distinct
// from the pattern one — §16.6 says the two rejections are "reached by
// different rules and their diagnostics differ; what agrees is the verdict".
bool hasInRangeNullableError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("left operand of `in` over a range is nullable") ==
                std::string::npos)
            continue;
        if (m.find("16.3") == std::string::npos) continue;
        if (m.find("7.11") == std::string::npos) continue;
        if (m.find("7.4") == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// ============================================================================
// Fail-open guard (§7.11 / proposal 0114 §1.1 / the "permission test passes on
// missing input" shape)
// ============================================================================

BEZEL_TEST(lifted_operators_rejected_3298, fail_open_guard_bitand_gt_zero) {
    // The literal example from proposal 0114 §1.1.
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var flags: uint? := null; "
        "  var mask: uint := 8u; "
        "  if (flags & mask) <> 0 then return 1; else return 0; end; "
        "end Main; end C;");
    // Pre-fix this compiled (lifted `flags & mask` → uint?; `null <> 0` → true);
    // post-fix `flags & mask` is the §7.11 compile error.
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, fail_open_guard_relational_negated) {
    // The `requires(not (retries > Max))` spelling — its positive form
    // already-faults, the negated form passed on null.
    auto r = parseSemaAll(
        "module M; class C public static method Main(b: int?): int "
        "  if not (b > 0) then return 1; else return 0; end; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

// ============================================================================
// Sticky accumulator (§6.6 / proposal 0114 §1.4)
// ============================================================================

BEZEL_TEST(lifted_operators_rejected_3298, sticky_accumulator_compound) {
    // The literal example from proposal 0114 §1.4.
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var total: int? := 0; "
        "  var p: int? := null; "
        "  total += p; "  // pre-fix: lifted silently to total := (total ?? 0) + p → null
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullableCompoundError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, sticky_accumulator_increment) {
    // `n++` desugars to `n := n + 1` (§17.6); with a nullable target the
    // underlying `+` is the §7.11 rejected form. Before this fix a local
    // `n++` over int? could ICE on `ConstantInt::get({i8, i32}, 1)`
    // (#2888) or silently drop the write.
    //
    // fix #3830 (v0.288): `var n: int? := 5;` narrows at its literal
    // initializer (§7.4), and narrowing now reaches `++`, so the ORIGINAL
    // local-variable spelling of this row is admitted post-fix — see
    // `narrowed_local_admits_increment` below. `n` is rewritten to a
    // PARAMETER here to keep testing the genuinely nullable, UNGUARDED gate
    // this row's name promises: a parameter is never narrowed by what the
    // caller passes (narrowing requires an in-body null test, §7.4).
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Chk(n: int?): int "
        "    n++; "
        "    return 0; "
        "  end Chk; "
        "  public static method Main(): int return Chk(5); end Main; "
        "end C;");
    // §17.6 names the remedies and the §6.6 desugaring; assert the TEXT, not
    // "some diagnostic fired", and scan ALL diagnostics rather than only the
    // first (an unrelated earlier error would otherwise make this vacuous).
    bool hit = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("`++`") == std::string::npos) continue;
        // §17.6's own wording: "an operand whose static type is a NULLABLE
        // TYPE" — the gate is not restricted to value types (a nullable record
        // that overloads `operator ++` is the case that proved it).
        if (m.find("not defined on a nullable type") == std::string::npos)
            continue;
        if (m.find("17.6") == std::string::npos) continue;
        if (m.find("7.11") == std::string::npos) continue;
        hit = true;
        break;
    }
    BEZEL_EXPECT_TRUE(hit);
}

BEZEL_TEST(lifted_operators_rejected_3298, sticky_accumulator_decrement) {
    // fix #3830: parameterized — see sticky_accumulator_increment above.
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Chk(n: int?): int "
        "    n--; "
        "    return 0; "
        "  end Chk; "
        "  public static method Main(): int return Chk(5); end Main; "
        "end C;");
    bool hit = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("`--`") == std::string::npos) continue;
        if (m.find("not defined on a nullable type") == std::string::npos)
            continue;
        if (m.find("17.6") == std::string::npos) continue;
        hit = true;
        break;
    }
    BEZEL_EXPECT_TRUE(hit);
}

// fix #3830 (v0.288): the row `sticky_accumulator_increment`/`_decrement`
// used to pin before they were rewritten to parameters above — a local's
// literal initializer §7.4-narrows it, and #3830 makes a narrowed `int?`
// target admitted at `++`/`--` exactly as at `+=`. Mirrors
// nullable_property_compound_2888_test.cpp's
// `local_increment_narrowed_accepted_3830` (gtest, which pins the VALUES;
// this sema-only file can only assert the absence of a diagnosis).
BEZEL_TEST(lifted_operators_rejected_3298, narrowed_local_admits_increment) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  n++; "
        "  n--; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ============================================================================
// All four operator families × the binary spelling they admit (§7.11)
// ============================================================================

BEZEL_TEST(lifted_operators_rejected_3298, arithmetic_plus_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int?) _ := a + b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, arithmetic_minus_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int?) _ := a - b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, arithmetic_times_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int?) _ := a * b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, arithmetic_div_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int?) _ := a / b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, arithmetic_mod_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int?) _ := a mod b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, bitwise_and_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int?) _ := a & b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, bitwise_or_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int?) _ := a | b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, bitwise_xor_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int?) _ := a ^ b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, shift_left_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int) _ := a << b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, shift_right_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int) _ := a >> b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, relational_lt_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int?) _ := a < b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, relational_le_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int?) _ := a <= b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, relational_gt_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int?) _ := a > b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, relational_ge_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int?) _ := a >= b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

// ============================================================================
// Mixed non-nullable/nullable operand pair — same rule, same diagnostic
// ============================================================================

BEZEL_TEST(lifted_operators_rejected_3298, mixed_nonnull_plus_nullable_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int, b: int?) _ := a + b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, mixed_nullable_plus_nonnull_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int) _ := a + b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

// ============================================================================
// Unary + / - / ~ over a nullable primitive (§3.5 / §7.11 / proposal D1)
// ============================================================================

BEZEL_TEST(lifted_operators_rejected_3298, unary_neg_nullable_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?) _ := -a; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, unary_plus_nullable_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?) _ := +a; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, unary_bitnot_nullable_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?) _ := ~a; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

// ============================================================================
// Compound forms over a nullable operand (§6.6 / #2153 widened by #3298)
// ============================================================================

BEZEL_TEST(lifted_operators_rejected_3298, compound_target_nullable) {
    // Pre-fix #2153 only rejected `int += int?` (target non-nullable,
    // rhs nullable). v0.267 widens this to ANY nullable operand.
    //
    // fix #3830 (v0.288): `var n: int? := 5;` narrows at its literal
    // initializer (§7.4), and narrowing now reaches compound assignment, so
    // the ORIGINAL local-variable spelling of this row is admitted post-fix
    // — see `narrowed_local_admits_compound` below. `n` is rewritten to a
    // PARAMETER here to keep testing the genuinely nullable, UNGUARDED
    // target this row's name promises: a parameter is never narrowed by
    // what the caller passes (narrowing requires an in-body null test,
    // §7.4).
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Chk(n: int?): int "
        "    n += 1; "
        "    return 0; "
        "  end Chk; "
        "  public static method Main(): int return Chk(5); end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNullableCompoundError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, compound_rhs_nullable) {
    // fix #3830: `n` is already non-nullable (unaffected); `m`'s literal
    // initializer narrows it (§7.4) — parameterized to keep testing the
    // genuinely nullable, unguarded RHS this row's name promises. See
    // compound_target_nullable above.
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Chk(m: int?): int "
        "    var n: int := 5; "
        "    n += m; "
        "    return 0; "
        "  end Chk; "
        "  public static method Main(): int return Chk(3); end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNullableCompoundError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, compound_both_nullable) {
    // fix #3830: both `n` and `m`'s literal initializers narrow them —
    // both parameterized to keep testing the "both nullable" verdict this
    // row names. See compound_target_nullable above.
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Chk(n: int?, m: int?): int "
        "    n += m; "
        "    return 0; "
        "  end Chk; "
        "  public static method Main(): int return Chk(5, 3); end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNullableCompoundError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, compound_and_eq_nullable) {
    // fix #3830: `n`'s literal initializer narrows it — parameterized. See
    // compound_target_nullable above.
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Chk(n: uint?): int "
        "    n &= 1u; "
        "    return 0; "
        "  end Chk; "
        "  public static method Main(): int return Chk(5u); end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNullableCompoundError(r));
}

// fix #3830 (v0.288): the rows above used to pin before they were rewritten
// to parameters — a local's literal initializer §7.4-narrows it, and #3830
// makes a narrowed `int?`/`uint?` operand admitted at compound assignment
// exactly as at the binary form. Mirrors
// nullable_property_compound_2888_test.cpp's
// `local_increment_narrowed_accepted_3830` (gtest, which pins VALUES; this
// sema-only file can only assert the absence of a diagnosis).
BEZEL_TEST(lifted_operators_rejected_3298, narrowed_local_admits_compound) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  n += 1; "
        "  var m: int? := 3; "
        "  n += m; "
        "  var u: uint? := 5u; "
        "  u &= 1u; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullableCompoundError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ============================================================================
// Float / decimal / other primitive kinds — same §7.11 rule
// ============================================================================

BEZEL_TEST(lifted_operators_rejected_3298, arithmetic_double_nullable_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: double?, b: double?) _ := a + b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, relational_double_nullable_rejected) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: double?, b: double?) _ := a < b; end M; end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

// ============================================================================
// §19.3 / §16.6 pattern companion — parity with `in` on a nullable scrutinee
// ============================================================================

BEZEL_TEST(lifted_operators_rejected_3298, case_relop_pattern_over_nullable_rejected) {
    // fix #3958: Appendix A's `CaseStmt ::= "case" Expression "of" CaseArm
    // ("," CaseArm)* ","? "end" "case"? ";"` comma-separates arms
    // unconditionally, so the first arm's trailing `,` after `return 1;` is
    // REQUIRED — this row omitted it and "passed" only via parseSemaAll's
    // pre-#3958 failure to gate on parse errors (the omission fed a
    // parser-error-recovery-truncated AST to sema). VERIFIED directly
    // against the real `bezelc -c`: with the comma restored, this source
    // parses with zero diagnostics and rejects at the relop pattern with
    // the expected §19.3/§16.6/§7.11 text.
    auto r = parseSemaAll(
        "module M; class C public static method Main(n: int?): int "
        "  case n of "
        "    > 30 => return 1;, "
        "    _ => return 0; "
        "  end case; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, case_range_pattern_over_nullable_rejected) {
    // fix #3958: same missing-comma defect as case_relop_pattern_over_
    // nullable_rejected above — see that test's comment. VERIFIED against
    // the real `bezelc -c` with the comma restored: parses cleanly, rejects
    // at the range pattern (§19.3/§16.6/§7.11).
    auto r = parseSemaAll(
        "module M; class C public static method Main(n: int?): int "
        "  case n of "
        "    1..5 => return 1;, "
        "    _ => return 0; "
        "  end case; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, case_relop_over_nullable_stmt_rejected) {
    // The statement-position `case` is the same gate at a different visit
    // site (ExprTypecheck.cpp ~line 14628); cover that path too.
    //
    // fix #3958: same missing-comma defect as the two rows above — see
    // case_relop_pattern_over_nullable_rejected's comment. VERIFIED against
    // the real `bezelc -c` with the comma restored: parses cleanly, rejects
    // at the relop pattern (§19.3/§16.6/§7.11).
    //
    // fix #5173 / §19.3 (v0.310, #5143 ruling): a §7.4-narrowed non-null read
    // of the nullable scrutinee is now ADMITTED. A bare parameter `n: int?`
    // is not narrowed (no `nullNarrowed_` entry) and therefore still rejects
    // at the relop pattern (the genuinely-un-narrowed case). The narrowed
    // variant is covered by the next test.
    auto r = parseSemaAll(
        "module M; class C public static method Main(n: int?): int "
        "  case n of "
        "    > 3 => return 1;, "
        "    _ => return 0; "
        "  end case; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

// fix #5173 / §19.3 (v0.310, #5143 ruling): the statement-position `case`
// over a §7.4-narrowed non-null read of a nullable scrutinee is ADMITTED.
// The narrowing context (`if n <> null`) puts `n` in `nullNarrowed_`, so
// `checkOrderingPatternsOverNullableScrutinee`'s `isNarrowedNonNullRead`
// gate (v0.310) lets the relop pattern through; codegen unwraps the
// narrowed `{i8 hasValue; i32 payload}` box in patternMatches's
// `isNullableVal` branch and runs the comparison against the inner value.
BEZEL_TEST(lifted_operators_rejected_3298,
           case_relop_over_narrowed_nullable_stmt_admitted_5173) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(n: int?): int "
        "  if n <> null then "
        "    case n of "
        "      > 3 => return 1;, "
        "      _ => return 0; "
        "    end case; "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullablePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}


// ---------------------------------------------------------------------------
// §19.3 (v0.286): "A **nullable** scrutinee — VALUE OR REFERENCE — is a
// compile error at a relational pattern, and at a range pattern ([§16.6])
// alike", and "The rejection is keyed on the **kind** of pattern, not on which
// bounds are written, so it reaches the open-ended forms and the fully open
// `..` with the closed ones."
//
// The first draft of this fix keyed the gate on `Primitive && nullable`, which
// admits only value types. MEASURED: with the backend's nv.relop.match /
// nv.range.match lowering deleted, a nullable CLASS-typed scrutinee then
// reached codegen and produced `icmp sgt ptr, i32` (malformed IR), and a
// nullable RECORD scrutinee produced an unterminated `case.end` block — a
// compiler ICE where §19.3 requires a diagnosis. These rows pin the widening.
// ---------------------------------------------------------------------------

BEZEL_TEST(lifted_operators_rejected_3298,
           case_relop_pattern_over_nullable_class_scrutinee_rejected) {
    auto r = parseSemaAll(
        "module M; class K public V: int := 0; end K; "
        "class C public static method Main(): int "
        "  var k: K? := null; "
        "  var v := (case k of > 3 => 1, _ => 2); "
        "  return v; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_range_pattern_over_nullable_record_scrutinee_rejected) {
    // fix #5173 / §19.3 (v0.310, #5143 ruling): a §7.4-narrowed non-null
    // read of a nullable record scrutinee is now ADMITTED (codegen
    // unwraps the `{i8 hasValue; <record payload>}` box in patternMatches's
    // `isNullableVal` branch — records are covered by isNullableValueTy).
    // A bare parameter that is not narrowed still rejects.
    auto r = parseSemaAll(
        "module M; record R(X: int); "
        "class C public static method Main(q: R?): int "
        "  var v := (case q of .. => 1, _ => 2); "
        "  return v; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

// fix #5173 / §19.3 (v0.310, #5143 ruling): narrowed record-scrutinee
// `case q of .. => 1, _ => 2` is admitted. patternMatches's `isNullableVal`
// path covers `record?` via `isNullableRecordTy` and unwraps with
// `nullablePrimValue` exactly as it does for a primitive.
BEZEL_TEST(lifted_operators_rejected_3298,
           case_range_pattern_over_narrowed_nullable_record_scrutinee_admitted_5173) {
    auto r = parseSemaAll(
        "module M; record R(X: int); "
        "class C public static method Main(q: R?): int "
        "  if q <> null then "
        "    var v := (case q of .. => 1, _ => 2); "
        "    return v; "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullablePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_relop_pattern_over_nullable_string_scrutinee_rejected) {
    // §7.11: "Nullable **reference** operands take the same answer at their
    // own types (§4.6 for `string`)" — and §19.3 says the same in pattern
    // position.
    //
    // fix #5173 / §19.3 (v0.310, #5143 ruling): narrowed non-null reads of
    // a nullable VALUE-typed scrutinee (primitive, enum, decimal, record,
    // choice) are ADMITTED. Reference types (`string?`) are OUT OF SCOPE:
    // patternMatches's reference-type relational comparison is not
    // implemented (it would `CreateICmpSLT` two `bz_string*` pointers — a
    // silent wrong value). The narrowed reference form is filed separately.
    // The test below covers the un-narrowed case (a bare parameter is not
    // narrowed and still rejects).
    auto r = parseSemaAll(
        "module M; class C public static method Main(s: string?): int "
        "  var v := (case s of > \"b\" => 1, _ => 2); "
        "  return v; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

// fix-forward #5173 / §19.3 / §7.11: the narrowed nullable reference-
// typed form (`if s <> null then case s of > "b" => …, …` over `string?`)
// is now REJECTED — the predicate at `valueTyScrut` excludes `string` and
// `Object` from the value-primitive family (the same exclusion codegen's
// `Emitter::isNullablePrimTy` enforces at LlvmBackend_Layout.cpp:755).
// Pre-fix-forward the predicate's Primitive arm admitted any
// `TypeRef::Kind::Primitive` regardless of `primitive`, so a narrowed
// `string?` reached codegen via patternMatches's NON-`isNullableVal` branch
// and lowered to `CreateICmpSLT(bz_string*, bz_string*)` — a SILENT WRONG
// VALUE (heap address, not §4.6 ordinal content). The narrowed-string
// reject is the only behavior flip on a reference-typed scrutinee; its
// UN-narrowed sibling (`case_relop_pattern_over_nullable_string_scrutinee
// _rejected` above) was already REJECTED by the loop walk and stays that
// way. The codegen path for the narrowed reference form is filed
// separately as issue #5220 (relop against `bz_string*` via
// `bz_str_compare_cp`, §4.6 ordinal) — once that lands, this predicate
// can be widened to admit `string?`/`Object?`. The narrowed-string/Object
// REJECT rows live in tests/sema/scrutinee_string_narrowed_relop_5173_
// test.cpp alongside the matching VALUE-typed control rows.

BEZEL_TEST(lifted_operators_rejected_3298,
           case_bare_open_range_pattern_over_nullable_rejected) {
    // The fully open `..`. §19.3 (v0.286) rejects it WITH the closed forms
    // because the rejection is keyed on the pattern's KIND: "a rule that
    // counted bounds would give a range pattern and another range pattern of
    // the same extent opposite verdicts".
    //
    // fix #5173 / §19.3 (v0.310, #5143 ruling): narrowed nullable scrutinees
    // are admitted. A bare parameter that is not narrowed still rejects.
    auto r = parseSemaAll(
        "module M; class C public static method Main(n: int?): int "
        "  var v := (case n of .. => 1, _ => 2); "
        "  return v; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

// fix #5173 / §19.3 (v0.310, #5143 ruling): narrowed bare-`..` over a
// nullable scrutinee is ADMITTED. `if n <> null then case n of .. => 1,
// _ => 2` compiles and selects the in-range arm.
BEZEL_TEST(lifted_operators_rejected_3298,
           case_bare_open_range_pattern_over_narrowed_nullable_admitted_5173) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(n: int?): int "
        "  if n <> null then "
        "    var v := (case n of .. => 1, _ => 2); "
        "    return v; "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullablePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_alternative_list_member_range_over_nullable_rejected) {
    // §19.3 admits ranges and relationals as binding-free ALTERNATIVE members
    // (`1..5 | 10 => …`), so the walk has to descend into an alternative list
    // or the rule is spelling-dependent inside one arm.
    //
    // fix #5173 / §19.3 (v0.310, #5143 ruling): a §7.4-narrowed non-null read
    // of the nullable scrutinee is now ADMITTED. A bare parameter is not
    // narrowed and still rejects.
    auto r = parseSemaAll(
        "module M; class C public static method Main(n: int?): int "
        "  var v := (case n of 1..5 | 10 => 1, _ => 2); "
        "  return v; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

// fix #5173 / §19.3 (v0.310, #5143 ruling): the alternative-list member
// form, narrowed — admitted.
BEZEL_TEST(lifted_operators_rejected_3298,
           case_alternative_list_member_range_over_narrowed_nullable_admitted_5173) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(n: int?): int "
        "  if n <> null then "
        "    var v := (case n of 1..5 | 10 => 1, _ => 2); "
        "    return v; "
        "  end if; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullablePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_constant_and_null_patterns_over_nullable_still_admitted) {
    // §19.3: "Constant patterns over a nullable scrutinee are unaffected: a
    // constant pattern is the equality test of §8.4, which is total on `null`."
    // The widened gate must not reach them (a LOUD->QUIET check in reverse:
    // the rejection must stay exactly as wide as §19.3 says).
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  var v := (case n of null => 0, 5 => 1, _ => 2); "
        "  return v; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullablePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §19.3 (v0.294, #3692): the ordering-pattern-over-nullable rule reaches a
// DESTRUCTURING SLOT's sub-pattern, not only the arm scrutinee. §19.3: "Each
// slot's right-hand side is a full pattern (grammar A.7 AltPattern): a
// binding, `_`, a constant, `null`, a range, a relational, … a per-slot `|`
// alternative … or a nested destructuring … recursively" — the relational
// paragraph's "nullable scrutinee" reads, for a slot, as the slot's own
// declared type. #3298 gated the ARM form only (walking arm patterns and
// alternative-list members); it did not resolve a destructuring pattern's
// per-slot declared type, so `case r of R(1..5, 1) => …` over `R(X: int?, …)`
// compiled and silently took proposal 0114's declined D5 reading instead of
// being rejected (codegen/case_relop_nullable_1074_test.cpp's
// `nullable_slot_*_3298` rows pinned that D5 table before this fix; they now
// pin the rejection, plus a residual-gap control for a GENERIC record's
// unsubstituted type-parameter slot — see that file's header and issue 3898).
//
// checkOrderingPatternOverNullableType is the ONE walk both the arm-level and
// slot-level call sites share (§6 dedup) — the same diagnostic,
// hasNullablePatternError, fires for both.
// ---------------------------------------------------------------------------

BEZEL_TEST(lifted_operators_rejected_3298,
           case_range_pattern_in_record_slot_over_nullable_field_rejected_3692) {
    // The issue's verbatim repro.
    auto r = parseSemaAll(
        "module M; record R(X: int?, Y: int); "
        "class C public static method Main(): int "
        "  var r := new R(99, 1); "
        "  return (case r of R(1..5, 1) => 7, _ => 3); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_relop_pattern_in_record_slot_over_nullable_field_rejected_3692) {
    auto r = parseSemaAll(
        "module M; record R(X: int?, Y: int); "
        "class C public static method Main(): int "
        "  var r := new R(99, 1); "
        "  return (case r of R(> 50, 1) => 7, _ => 3); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_range_pattern_in_record_slot_over_nullable_field_stmt_rejected_3692) {
    // Statement-position `case` — a separate visit site from the expression
    // form, exactly as the arm-level gate's own stmt row covers.
    //
    // fix #3958: a FIFTH row with the same missing-comma defect the issue
    // pins (found while fixing the four the issue lists) — see
    // case_relop_pattern_over_nullable_rejected's comment above for the
    // grammar citation. VERIFIED against the real `bezelc -c` with the
    // comma restored: parses cleanly, rejects at the range pattern
    // (§19.3/§16.6/§7.11).
    auto r = parseSemaAll(
        "module M; record R(X: int?, Y: int); "
        "class C public static method Main(): int "
        "  var r := new R(99, 1); "
        "  case r of "
        "    R(1..5, 1) => return 7;, "
        "    _ => return 3; "
        "  end case; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_range_pattern_in_choice_payload_slot_over_nullable_field_rejected_3692) {
    // A choice-case payload slot — the OTHER of the "record x choice" halves
    // the issue names; this path already substitutes a generic instantiation
    // via `substituteGenericFieldType` (fix #3898 generalized this from the
    // choice-only `substituteGenericCaseFieldType` #2219 introduced), so it
    // was already fully closed here even before #3898 — the record path's
    // own generic residual (issue 3898's TOP-LEVEL repro, below) is now
    // closed too; the NESTED generic residual that #3898 left open as
    // issue 3938 is itself closed by fix #3938 (the
    // `..._rejected_3938` rows below).
    auto r = parseSemaAll(
        "module M; choice Wrap Val(X: int?, Y: int), None; end Wrap; "
        "class C public static method Main(): int "
        "  var w := Wrap.Val(99, 1); "
        "  return (case w of Val(1..5, 1) => 7, _ => 3); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_range_pattern_in_nested_record_slot_over_nullable_field_rejected_3692) {
    // Two levels deep (`Outer(Inner(1..5), b)`) — a separate call site (the
    // nested-destructuring worklist) from the top-level slot loop above.
    auto r = parseSemaAll(
        "module M; record Inner(V: int?); record Outer(I: Inner, B: int); "
        "class C public static method Main(): int "
        "  var o := new Outer(new Inner(99), 1); "
        "  return (case o of Outer(Inner(1..5), 1) => 7, _ => 3); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_alternative_list_member_range_in_slot_over_nullable_field_rejected_3692) {
    // §19.3 admits ranges/relationals as binding-free ALTERNATIVE members
    // inside a slot too (`R(1..5 | 10, 1)`); the walk must descend into the
    // slot's alternative list, not just an arm's.
    auto r = parseSemaAll(
        "module M; record R(X: int?, Y: int); "
        "class C public static method Main(): int "
        "  var r := new R(99, 1); "
        "  return (case r of R(1..5 | 10, 1) => 7, _ => 3); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_constant_slot_over_nullable_field_still_admitted_3692) {
    // §19.3: a constant slot is unaffected — it is §8.4 equality, total on
    // `null`. LOUD->QUIET guard: the widened gate must stay exactly as wide
    // as §19.3 says.
    auto r = parseSemaAll(
        "module M; record R(X: int?, Y: int); "
        "class C public static method Main(): int "
        "  var r := new R(5, 1); "
        "  return (case r of R(5, 1) => 7, _ => 3); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullablePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_range_pattern_in_generic_record_slot_over_nullable_field_rejected_3898) {
    // fix #3898 (§14.1 / §19.3, v0.294): CLOSED. This is the issue's own
    // verbatim repro. Before the fix, a generic record's TOP-LEVEL slot was
    // typed at the bare, unsubstituted type parameter from sema's point of
    // view (there was no record analogue of the choice path's
    // `substituteGenericCaseFieldType`), so `Box<int?>`'s slot-0 declared
    // type was seen as non-nullable `T`, not `int?`, and this program was
    // wrongly ADMITTED (pinned as `..._admitted_3692` on the parent
    // compiler). `ExprTC::substituteGenericFieldType` — the same walk,
    // generalized to accept a Record declaration alongside Choice — is now
    // called at the record-scrutinee slot-resolution sites too, so the slot
    // resolves to the substituted `int?` and this rejects, symmetric with
    // the non-generic `R(1..5, 1)` row above.
    auto r = parseSemaAll(
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var b := new Box<int?>(99, 1); "
        "  return (case b of Box(1..5, 1) => 7, _ => 3); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_relop_pattern_in_generic_record_slot_over_nullable_field_rejected_3898) {
    // fix #3898: the RELATIONAL form of the same generic-field shape.
    auto r = parseSemaAll(
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var b := new Box<int?>(10, 1); "
        "  return (case b of Box(> 5, 1) => 7, _ => 3); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_range_pattern_in_generic_record_slot_over_nullable_field_stmt_rejected_3898) {
    // fix #3898: statement-position `case` — the CaseStmt sema handler is a
    // separate call site from CaseExpr's, and needs its OWN
    // `substituteGenericFieldType` call (which the fix adds).
    //
    // Appendix A: `CaseStmt ::= "case" Expression "of" CaseArm ("," CaseArm)*
    // ","? "end" "case"? ";"` — arms are comma-separated, unconditionally, so
    // the first arm's trailing `,` after its `return 7;` is REQUIRED, not
    // stylistic (a sibling defect, several rows in this file omitting it, is
    // tracked separately as issue 3958 — this row was written from that
    // defective template and is corrected here). VERIFIED directly against
    // the real `bezelc -c`: this exact source parses with zero diagnostics
    // through the `case` construct, then rejects at the range pattern with
    // the expected §19.3/§16.6/§7.11 text — genuine sema coverage, not a
    // parser-error-recovery artifact.
    auto r = parseSemaAll(
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var b := new Box<int?>(3, 1); "
        "  case b of "
        "    Box(1..5, 1) => return 7;, "
        "    _ => return 3; "
        "  end case; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_range_pattern_in_generic_record_second_type_param_slot_rejected_3898) {
    // fix #3898: a SECOND generic parameter position (`Pair<A,B>`, ordering
    // on B) — the substitution walk must match B by NAME against the
    // scrutinee's genericArgs, not just special-case "the first parameter".
    auto r = parseSemaAll(
        "module M; record Pair<A, B>(X: A, Y: B); "
        "class C public static method Main(): int "
        "  var p := new Pair<int, int?>(1, 3); "
        "  return (case p of Pair(1, 1..5) => 7, _ => 3); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_range_pattern_in_generic_record_slot_non_nullable_instantiation_still_admitted_3898) {
    // POSITIVE CONTROL (§19.3 stays exactly as wide as it says): the SAME
    // generic-record slot shape instantiated at a NON-nullable type argument
    // (`Box<int>`) must remain ADMITTED — the fix keys on the SUBSTITUTED
    // type's own nullability, not on "the slot's declared type names a
    // generic parameter" (which would over-reject every generic-record
    // ordering slot, nullable substitution or not).
    auto r = parseSemaAll(
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var b := new Box<int>(3, 1); "
        "  return (case b of Box(1..5, 1) => 7, _ => 3); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullablePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_range_pattern_in_nested_generic_record_slot_rejected_3938) {
    // fix #3938 (§19.3 / §14.1, v0.294): CLOSED. A generic record slot
    // NESTED one level down inside another destructuring pattern
    // (`Outer<T>(B: Box<T>, Z: int)`) used to go through
    // `ExprTC::bindNestedDestructuringNames`, which resolved each nested
    // pattern's record purely by NAME and had no substitution context of
    // any kind — so `Box`'s own bare `T` stayed unsubstituted and this
    // program was (wrongly, per §19.3) ADMITTED. The fix threads each
    // pending nested pattern's own already-substituted scrutTy through
    // the worklist (the parent slot's substituted type, computed by the
    // caller loop via #3898's `substituteGenericFieldType`), so `Box`'s
    // bare `T` is now substituted against `Outer<int?>`'s instantiation
    // and this REJECTS, symmetric with the non-generic
    // `case_range_pattern_in_nested_record_slot_over_nullable_field_rejected_3692`
    // above and with the top-level
    // `case_range_pattern_in_generic_record_slot_over_nullable_field_rejected_3898`
    // three sections above.
    auto r = parseSemaAll(
        "module M; record Box<T>(X: T, Y: int); "
        "record Outer<T>(B: Box<T>, Z: int); "
        "class C public static method Main(): int "
        "  var o := new Outer<int?>(new Box<int?>(99, 1), 9); "
        "  return (case o of Outer(Box(1..5, 1), 9) => 7, _ => 3); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_range_pattern_in_generic_choice_payload_nested_record_slot_rejected_3938) {
    // fix #3938 (§19.3 / §14.1, v0.294): CLOSED — the identical nested
    // shape via a CHOICE-case payload. The choice-scrutinee caller loop
    // now hands each nested pattern its own already-substituted `fieldTy`
    // (the substituted payload type, computed via
    // `substituteGenericFieldType(fieldTy, scrutTy)`, #2219's
    // pre-existing choice-side substitution), so `Box`'s bare `T` is
    // substituted against `Wrap<int?>`'s instantiation too and this
    // REJECTS — symmetric with the top-level
    // `case_range_pattern_in_choice_payload_slot_over_nullable_field_rejected_3692`
    // above.
    auto r = parseSemaAll(
        "module M; record Box<T>(X: T, Y: int); "
        "choice Wrap<T> Val(B: Box<T>), None; end Wrap; "
        "class C public static method Main(): int "
        "  var w := Wrap<int?>.Val(new Box<int?>(99, 1)); "
        "  return (case w of Val(Box(1..5, 1)) => 7, _ => 3); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

// ---------------------------------------------------------------------------
// §17.5 / §16.6 — `x in <range>` with a nullable left operand, and the §16.6
// PARITY the issue's "Done means" asks for.
// ---------------------------------------------------------------------------

BEZEL_TEST(lifted_operators_rejected_3298, in_range_literal_nullable_left_rejected) {
    // A PARAMETER, deliberately: `var n: int? := 5;` narrows the local at its
    // initializer (§7.4), and §7.11's static-type reading then makes it a
    // non-nullable operand `Range<T>.Contains(T)` legitimately admits. §17.5's
    // rule is about the un-narrowed operand.
    auto r = parseSemaAll(
        "module M; class C public static method Probe(n: int?): bool "
        "  return n in 1..5; "
        "end Probe; end C;");
    BEZEL_EXPECT_TRUE(hasInRangeNullableError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, in_range_variable_nullable_left_rejected) {
    // The range-typed-variable spelling reaches the same gate.
    auto r = parseSemaAll(
        "module M; class C public static method Probe(n: int?): bool "
        "  var rg := 1..5; "
        "  return n in rg; "
        "end Probe; end C;");
    BEZEL_EXPECT_TRUE(hasInRangeNullableError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, in_range_narrowed_left_admitted) {
    // §7.11's static-type reading applies here too: a §7.4-narrowed operand is
    // a non-nullable operand, so `Range<T>.Contains(T)` admits it. The codegen
    // half of the pair unwraps the `{i8, T}` box (see the `in` lowering).
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  var b: bool := false; "
        "  if n <> null then b := n in 1..10; end; "
        "  return (if b then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasInRangeNullableError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(lifted_operators_rejected_3298, section_16_6_parity_in_and_case_agree) {
    // §16.6's claim, now true: `n in 1..5` and `case n of 1..5` over an `int?`
    // give the SAME VERDICT. §16.6 also says the two rejections are "reached
    // by different rules and their diagnostics differ", so this asserts the
    // verdict agrees while the two distinct diagnostics each fire at their own
    // site — not that one message serves both.
    auto inForm = parseSemaAll(
        "module M; class C public static method Probe(n: int?): bool "
        "  return n in 1..5; "
        "end Probe; end C;");
    auto caseForm = parseSemaAll(
        "module M; class C public static method Probe(n: int?): int "
        "  return (case n of 1..5 => 1, _ => 2); "
        "end Probe; end C;");
    BEZEL_EXPECT_TRUE(inForm.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(caseForm.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInRangeNullableError(inForm));
    BEZEL_EXPECT_TRUE(hasNullablePatternError(caseForm));
    // …and neither rule leaks into the other's site.
    BEZEL_EXPECT_FALSE(hasNullablePatternError(inForm));
    BEZEL_EXPECT_FALSE(hasInRangeNullableError(caseForm));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           section_19_3_pattern_position_performs_no_narrowing) {
    // §17.5 / §19.3 NOW AGREE on a §7.4-narrowed non-null read of a nullable
    // scrutinee, after fix #5173 (v0.310, #5143 ruling). v0.286 read §19.3
    // as "pattern position performs no narrowing" and the case form was the
    // one place §17.5 and §19.3 DELIBERATELY DISAGREED — the case form was
    // rejected at a range pattern even where §7.4 had proved the operand
    // non-null, while the same narrowing DID rescue `n in 1..5` via §7.11's
    // static-type reading (§17.5 defers to §7.11 through §5.1).
    //
    // v0.310 collapses the disagreement. §19.3 now reads the scrutinee's
    // nullability off the static type AFTER §7.4 narrowing, as §7.11 does;
    // the pattern rule keys on `isNarrowedNonNullRead(scrutinee)` rather
    // than on the declared-null flag alone. §16.6's parity claim "over a
    // nullable `v`" — un-narrowed — is preserved by the bare-parameter reject
    // (still pinned by case_range_pattern_over_nullable_rejected, the
    // _rejected rows, and the lit companion at
    // tests/lit/sema/lifted_ops_3298__range_pattern_bare_open_nullable_rejected.bzl).
    auto narrowedIn = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  var b: bool := false; "
        "  if n <> null then b := n in 1..5; end; "
        "  return (if b then 1 else 0); "
        "end Main; end C;");
    auto narrowedCase = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  var v := 0; "
        "  if n <> null then v := (case n of 1..5 => 1, _ => 2); end; "
        "  return v; "
        "end Main; end C;");
    // §7.11's reading (narrowed `n in 1..5`): admitted.
    BEZEL_EXPECT_FALSE(narrowedIn.diagnostics.hasErrors());
    // §19.3 (v0.310): the same narrowing now ALSO admits the case form.
    BEZEL_EXPECT_FALSE(hasNullablePatternError(narrowedCase));
    BEZEL_EXPECT_FALSE(narrowedCase.diagnostics.hasErrors());
}


// ---------------------------------------------------------------------------
// §7.4's TWO narrowable storages (v0.286): a simple local/parameter AND a
// `readonly` field. §7.4 licenses the field case on the ground that a readonly
// field "has no writer on any path, through any alias" after construction, so
// there is no TOCTOU hazard — v0.214/#2085/#2116 implement it as
// `fieldNarrowed_`. A draft of #3298 consulted only the local sets and its
// comment asserted "§7.4 narrows simple variables only", which is false; the
// consequence was a false reject on the guarded field read below.
// ---------------------------------------------------------------------------

BEZEL_TEST(lifted_operators_rejected_3298, narrowed_readonly_field_admits_arith) {
    auto r = parseSemaAll(
        "module M; class C "
        "  private readonly _n: int? := 5; "
        "  public method Go(): int "
        "    var m: int := 0; "
        "    if _n <> null then m := _n + 1; end if; "
        "    return m; "
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasNullableOpError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(lifted_operators_rejected_3298, unguarded_readonly_field_still_rejected) {
    // The control: without the `<> null` guard the field is a nullable operand
    // and §7.11 rejects it. A gate that admitted every readonly field read
    // would pass the row above vacuously.
    auto r = parseSemaAll(
        "module M; class C "
        "  private readonly _n: int? := 5; "
        "  public method Go(): int => _n + 1; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasNullableOpError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, mutable_field_never_narrows_and_is_rejected) {
    // §7.4 narrows neither a MUTABLE field nor a property — the TOCTOU hazard
    // the readonly rule exists to exclude — so the guard does not rescue it.
    auto r = parseSemaAll(
        "module M; class C "
        "  private _n: int? := 5; "
        "  public method Go(): int "
        "    var m: int := 0; "
        "    if _n <> null then m := _n + 1; end if; "
        "    return m; "
        "  end Go; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §17.6 (v0.286): "An operand whose static type is a NULLABLE TYPE is a
// compile error" — a nullable type, not a nullable *value* type. A draft of
// #3298 gated on `Primitive` and placed the test AFTER §17.6's three-category
// dispatch, so a nullable RECORD that declares `operator ++` matched the
// overload branch, was admitted, and died in codegen ("Call parameter type
// does not match function signature").
// ---------------------------------------------------------------------------

BEZEL_TEST(lifted_operators_rejected_3298, increment_on_nullable_record_with_overload_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "record Counter(N: int) "
        "  public static method operator ++(a: Counter): Counter "
        "    => new Counter(a.N + 1); "
        "end Counter; "
        "class C public static method Main(): int "
        "  var m: Counter? := new Counter(1); "
        "  m++; "
        "  return 0; "
        "end Main; end C;");
    bool hit = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("`++`") == std::string::npos) continue;
        if (m.find("is not defined on a nullable type") == std::string::npos)
            continue;
        if (m.find("17.6") == std::string::npos) continue;
        hit = true;
        break;
    }
    BEZEL_EXPECT_TRUE(hit);
}

// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix comment (kept for
// history): "The control: §17.6 admits `x++` on a record that overloads the
// operator. The nullability test above must not have swallowed that."
// §25.1/§17.6 retire that admission entirely — `operator ++` is rejected at
// its own declaration, and the non-nullable `m++` use is independently
// rejected too. The test ABOVE this one
// (`increment_on_nullable_record_with_overload_rejected`) is UNCHANGED: its
// nullable-type reject fires before this branch is ever reached, so it is
// unaffected by this fix either way.
BEZEL_TEST(lifted_operators_rejected_3298,
           increment_on_nonnullable_record_with_overload_rejected_spec_25_1_issue_3978) {
    auto r = parseSemaAll(
        "module M; "
        "record Counter(N: int) "
        "  public static method operator ++(a: Counter): Counter "
        "    => new Counter(a.N + 1); "
        "end Counter; "
        "class C public static method Main(): int "
        "  var m: Counter := new Counter(1); "
        "  m++; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool citedDecl = false, citedUse = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("operator `++` may not be overloaded (§25.1)") !=
            std::string::npos)
            citedDecl = true;
        if (d.message.find("`++` is not defined on record type `Counter`") !=
            std::string::npos)
            citedUse = true;
    }
    BEZEL_EXPECT_TRUE(citedDecl);
    BEZEL_EXPECT_TRUE(citedUse);
}

// ---------------------------------------------------------------------------
// §7.4 / §5.3 (v0.286): the establishment predicate must read the VALUE, not
// only its type. §5.3 constant-narrowing retypes a literal to the target type,
// so `var a: byte? := 3;` carried a NULLABLE initializer type and established
// no narrowing, while `var a: int? := 3;` did — §7.11 then answered
// differently for the two at identical shapes, decided by the operand's width.
// ---------------------------------------------------------------------------

// fix #4930 (v0.315): init-narrowing (§6.6) proves a local non-null for
// member access but NOT for the §7.11 operator non-lift gate. Only null-test
// narrowing (§7.4, `if a <> null then`) admits arithmetic/relational operators
// on nullable operands. The test below was pinning the OLD behavior.
// BEHAVIOR FLIP: was EXPECT_FALSE (init-narrowing admitted op), now
// EXPECT_TRUE (init-narrowing does NOT admit operators, only null-test narrowing does).
BEZEL_TEST(lifted_operators_rejected_3298,
           init_narrowing_does_not_admit_arithmetic_on_nullable_4930) {
    const char* widths[] = {"byte", "sbyte", "short", "ushort",
                            "int", "long", "double", "float", "decimal"};
    for (const char* w : widths) {
        auto r = parseSemaAll(
            std::string("module M; class C public static method Main(): int "
                        "  var a: ") + w + "? := 3; "
            "  var b := a + a; "
            "  return 0; "
            "end Main; end C;");
        // §7.11 (v0.315, #4930): init-narrowing does NOT admit operators.
        BEZEL_EXPECT_TRUE(hasNullableOpError(r));
    }
}

BEZEL_TEST(lifted_operators_rejected_3298, null_initializer_narrows_at_no_width) {
    const char* widths[] = {"byte", "sbyte", "short", "ushort",
                            "int", "long", "double", "float", "decimal"};
    for (const char* w : widths) {
        auto r = parseSemaAll(
            std::string("module M; class C public static method Main(): int "
                        "  var a: ") + w + "? := null; "
            "  var b := a + a; "
            "  return 0; "
            "end Main; end C;");
        BEZEL_EXPECT_TRUE(hasNullableOpError(r));
    }
}

// ============================================================================
// RETAINED set (§8.4 equality, §7.5 `?.`, §7.6 `?(…)`, §7.7 `??`, `??=`).
// Each must still compile, including the traced compound-after-`??=` shape.
// ============================================================================

BEZEL_TEST(lifted_operators_rejected_3298, retained_eq_nullable) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int?) _ := a = b; end M; end C;");
    BEZEL_EXPECT_FALSE(hasNullableOpError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(lifted_operators_rejected_3298, retained_lt_nullable) {
    // = / <> are exempted; <  is the relational §7.11 rule.
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int?) _ := a <> b; end M; end C;");
    BEZEL_EXPECT_FALSE(hasNullableOpError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(lifted_operators_rejected_3298, retained_qq_nullable) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?) _ := a ?? 0; end M; end C;");
    BEZEL_EXPECT_FALSE(hasNullableOpError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(lifted_operators_rejected_3298, retained_qq_eq_nullable_target) {
    // `n ??= 0` on an `int?` is NOT a read-modify-write (§6.6); it must
    // remain accepted even though the target is nullable.
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  n ??= 0; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullableCompoundError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// fix #3830 (v0.288): ⚠ THIS ROW FLIPPED. It used to pin proposal 0114's
// "Done means" trace case verbatim:
//   n ??= 0; n += 1;
// with the ISSUE'S OWN wording "must now reject only at the `+=`" — i.e. the
// `??=` is accepted (§6.6's structural exclusion, unaffected by this fix) but
// the subsequent `+=` was expected to STILL REJECT, because lifting §7.7's
// "after `x ??= expr`, x is narrowed non-null" (§7.4 v0.210, #2104 — a
// PRE-EXISTING, unrelated rule: `narrowOnAssignment` already treats `??=`
// exactly like `:=` for narrowing purposes, ExprTypecheck.cpp ~15357) onto a
// SUBSEQUENT statement was "out of scope — proposal 0114 doesn't rule it" at
// the time this test was written.
//
// #3830 (v0.288, ruling on issue 3686) is exactly that missing ruling for the
// compound-assignment READ side: it makes `n += 1` consult §7.4 narrowing at
// all, and `n`'s narrowing here was ALREADY being established by #2104's
// `??=` rule (`0`'s static type is non-nullable, so every path leaves `n`
// non-null) — #3830 did not touch narrowing ESTABLISHMENT, only where it is
// READ, so composing the two now means the trace is admitted end to end.
// Verified empirically: the program computes 1 (`n` becomes 0 via `??=`,
// then 1 via `+=`) — confirmed via `bezelc -o` + run, not merely "no error".
// A parameter does NOT rescue this row the way it does the LITERAL-narrowing
// fallout elsewhere in this file: `??=` narrows via an IN-BODY event exactly
// as `if n <> null` does, and that reaches a parameter identically to a
// local (only a narrowing-via-CALLER-supplied-value, which parameters are
// immune to, does not apply here) — verified: the parameter form admits too.
BEZEL_TEST(lifted_operators_rejected_3298, qq_eq_then_compound_narrowed_admitted_3830) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := null; "
        "  n ??= 0; "
        "  n += 1; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullableCompoundError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The control this file's original test also needed: `??=` narrows only when
// its RHS is non-nullable (§7.4 v0.210's own condition — "on the was-null-
// then-null path x leaves null, which narrowOnAssignment handles by
// clearing"). A NULLABLE `??=` RHS establishes no narrowing, so the
// subsequent `+=` is still the ordinary §7.11 reject — this is the row that
// keeps testing what `retained_qq_eq_then_compound` used to name: `??=` is
// retained/accepted on its own line, and an UNNARROWED target still rejects
// at the following compound form.
BEZEL_TEST(lifted_operators_rejected_3298, qq_eq_with_nullable_rhs_leaves_target_unnarrowed) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := null; "
        "  var m: int? := null; "
        "  n ??= m; "
        "  n += 1; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    int compoundCount = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("compound assignment") != std::string::npos &&
            d.message.find("7.11") != std::string::npos &&
            d.message.find("6.6") != std::string::npos)
            ++compoundCount;
    }
    // Exactly one diagnostic at the `+=` line — the §7.11 compound reject.
    BEZEL_EXPECT_EQ(compoundCount, 1);
}

// ============================================================================
// Narrowing §7.4 — `x <> null` (or `??`) before the operator keeps the operand
// non-nullable and the ordinary operator applies.
// ============================================================================

BEZEL_TEST(lifted_operators_rejected_3298, narrowed_local_admits_arith) {
    // §7.11: "an operand that §7.4 has narrowed non-null is a NON-NULLABLE
    // operand and the ordinary operator applies to it unchanged". So the SUM
    // is `int`, not `int?` — the store into a non-nullable `m` is the oracle
    // that observes the RESULT TYPE, which `EXPECT_FALSE(hasNullableOpError)`
    // alone cannot. The first draft of this fix rejected exactly this program
    // ("the assigned value is nullable but the target type is non-nullable"),
    // and no test in the tree caught it.
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  var m: int := 0; "
        "  if n <> null then m := n + 1; end; "
        "  return m; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullableOpError(r));
    BEZEL_EXPECT_FALSE(hasNullableCompoundError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(lifted_operators_rejected_3298, narrowed_local_admits_mixed_widening) {
    // §6.2 promotion has to see the NORMALIZED operand types too: a narrowed
    // `int?` plus a `long` is a `long`, not a false reject and not an `int?`.
    // The §6.2 widening block is guarded on `!lt->nullable && !rt->nullable`,
    // so an un-normalized narrowed operand skipped promotion entirely.
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  var w: long := 7; "
        "  var acc: long := 0; "
        "  if n <> null then acc := n + w; end; "
        "  return int(acc); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullableOpError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(lifted_operators_rejected_3298, narrowed_local_admits_unary) {
    // Same rule at the unary operators: `-n` over a narrowed `int?` is an
    // `int`, so it stores into a non-nullable target.
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  var m: int := 0; "
        "  if n <> null then m := -n; end; "
        "  return m; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullableOpError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(lifted_operators_rejected_3298, narrowed_local_admits_relational) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  if n <> null then "
        "    if n > 0 then return 1; end; "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullableOpError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ============================================================================
// Diagnostic must NAME A REMEDY — the user has to see one of §7.4 / `??` / `!`
// in the message, otherwise the rejection technically fires but the author
// is left guessing.
// ============================================================================

BEZEL_TEST(lifted_operators_rejected_3298, diagnostic_names_three_remedies) {
    auto r = parseSemaAll(
        "module M; class C public method M(a: int?, b: int?) _ := a + b; end M; end C;");
    bool sawAll = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("7.4") == std::string::npos) continue;
        if (d.message.find("`??`") == std::string::npos) continue;
        if (d.message.find("`!`") == std::string::npos) continue;
        sawAll = true;
        break;
    }
    BEZEL_EXPECT_TRUE(sawAll);
}

// ============================================================================
// §7.11 / proposal 0114 D8 (#3298): the user-defined-operator gate produces
// THE SAME diagnostic as the primitive gate — same wording, same remedies
// (per §7.11 v0.277: "such an operand at a user-defined-operator position
// is the same compile error, naming the same remedies"). The pre-fix
// wording was "is deferred / unwrap with `!` or `??`" which neither matched
// the primitive gate's exact text nor reflected the post-v0.267 ruling.
// This test pins both halves of the fix.
// ============================================================================

BEZEL_TEST(lifted_operators_rejected_3298, user_operator_gate_mirrors_primitive_gate_v0_267) {
    // A record V declares an overloadable `operator+` on non-nullable V;
    // a nullable V? operand at the call site exercises the user-operator
    // gate at ExprTypecheck_Exprs.cpp:4443-4482 (the pre-v0.267 "DEFERRED"
    // wording). The post-v0.267 wording must match the primitive gate's
    // "is not defined on a nullable value type — narrow first (§7.4, ?? §7.7,
    // ! §7.8)" framing with the explicit "user-defined operators do not
    // lift over `T?` (§7.11, v0.267)" clause.
    //
    // fix #3958: a SIXTH row with a parseSemaAll-masked grammar-invalid
    // defect, found probing the whole file for parse errors — distinct
    // from the case-comma family. `class C public method M(...): V => a +
    // b;` is Appendix A's arrow `MethodBody ::= "=>" (Expression |
    // AssignmentStmt) ";"` alternative (v0.294), which is complete at the
    // `;` and takes no trailing `end Identifier? ;` (that belongs only to
    // the `Statement* "end" Identifier? ";"` block-body alternative). The
    // stray `end M;` this row appended did not close the method (already
    // terminated) and mismatched class C's own end-label check, producing
    // a real parse error masked by parseSemaAll's pre-#3958 failure to gate
    // on it. VERIFIED against the real `bezelc -c`: with `end M;` removed,
    // this source parses with zero diagnostics and rejects at the `+` with
    // the expected §7.11/v0.267 text.
    auto r = parseSemaAll(
        "module M; "
        "record V(n: int) "
        "  public static method operator+(a: V, b: V): V => new V(a.n + b.n); "
        "end V; "
        "class C public method M(a: int, b: V?): V => a + b; "
        "end C;");
    bool hit = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        // Must say "rejected" (not "deferred") and use the same phrasing
        // as the primitive gate. The user-operator gate's specific clause
        // is "user-defined operators do not lift over `T?` (§7.11, v0.267)".
        if (d.message.find("user-defined operators do not lift over `T?`") ==
                std::string::npos)
            continue;
        if (d.message.find("is not defined on a nullable value type") ==
                std::string::npos)
            continue;
        if (d.message.find("§7.4") == std::string::npos) continue;
        if (d.message.find("`??`") == std::string::npos) continue;
        if (d.message.find("`!`") == std::string::npos) continue;
        if (d.message.find("deferred") != std::string::npos) {
            // The OLD pre-v0.267 "deferred" wording would have been a
            // regression on D8 — fail loudly if it reappears.
            continue;
        }
        hit = true;
        break;
    }
    BEZEL_EXPECT_TRUE(hit);
}
