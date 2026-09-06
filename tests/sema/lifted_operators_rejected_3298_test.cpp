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
// the `retained_*` tests; the trace case `n ??= 0; n += 1;` (the issue's
// "must reject only at the `+=`" example) is the signature acceptance test.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
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
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  n++; "
        "  return 0; "
        "end Main; end C;");
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
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  n--; "
        "  return 0; "
        "end Main; end C;");
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
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  n += 1; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullableCompoundError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, compound_rhs_nullable) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int := 5; "
        "  var m: int? := 3; "
        "  n += m; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullableCompoundError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, compound_both_nullable) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  var m: int? := 3; "
        "  n += m; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullableCompoundError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, compound_and_eq_nullable) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: uint? := 5u; "
        "  n &= 1u; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullableCompoundError(r));
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
    auto r = parseSemaAll(
        "module M; class C public static method Main(n: int?): int "
        "  case n of "
        "    > 30 => return 1; "
        "    _ => return 0; "
        "  end case; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, case_range_pattern_over_nullable_rejected) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(n: int?): int "
        "  case n of "
        "    1..5 => return 1; "
        "    _ => return 0; "
        "  end case; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298, case_relop_over_nullable_stmt_rejected) {
    // The statement-position `case` is the same gate at a different visit
    // site (ExprTypecheck.cpp ~line 14628); cover that path too.
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  case n of "
        "    > 3 => return 1; "
        "    _ => return 0; "
        "  end case; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
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
    auto r = parseSemaAll(
        "module M; record R(X: int); "
        "class C public static method Main(): int "
        "  var q: R? := new R(1); "
        "  var v := (case q of .. => 1, _ => 2); "
        "  return v; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_relop_pattern_over_nullable_string_scrutinee_rejected) {
    // §7.11: "Nullable **reference** operands take the same answer at their
    // own types (§4.6 for `string`)" — and §19.3 says the same in pattern
    // position.
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var s: string? := \"a\"; "
        "  var v := (case s of > \"b\" => 1, _ => 2); "
        "  return v; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_bare_open_range_pattern_over_nullable_rejected) {
    // The fully open `..`. §19.3 (v0.286) rejects it WITH the closed forms
    // because the rejection is keyed on the pattern's KIND: "a rule that
    // counted bounds would give a range pattern and another range pattern of
    // the same extent opposite verdicts".
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  var v := (case n of .. => 1, _ => 2); "
        "  return v; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_alternative_list_member_range_over_nullable_rejected) {
    // §19.3 admits ranges and relationals as binding-free ALTERNATIVE members
    // (`1..5 | 10 => …`), so the walk has to descend into an alternative list
    // or the rule is spelling-dependent inside one arm.
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := 5; "
        "  var v := (case n of 1..5 | 10 => 1, _ => 2); "
        "  return v; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
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
    auto r = parseSemaAll(
        "module M; record R(X: int?, Y: int); "
        "class C public static method Main(): int "
        "  var r := new R(99, 1); "
        "  case r of "
        "    R(1..5, 1) => return 7; "
        "    _ => return 3; "
        "  end case; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasNullablePatternError(r));
}

BEZEL_TEST(lifted_operators_rejected_3298,
           case_range_pattern_in_choice_payload_slot_over_nullable_field_rejected_3692) {
    // A choice-case payload slot — the OTHER of the "record x choice" halves
    // the issue names; this path already substitutes a generic instantiation
    // via substituteGenericCaseFieldType, so it is fully closed (unlike the
    // record path's generic residual, issue 3898).
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
           case_range_pattern_in_generic_record_slot_over_nullable_field_admitted_3692) {
    // The RESIDUAL gap (issue 3898): a generic record's slot is typed at the
    // bare, unsubstituted type parameter from sema's point of view (there is
    // no record analogue of substituteGenericCaseFieldType), so `Box<int?>`'s
    // slot-0 declared type is seen as non-nullable `T`, not `int?`, and this
    // program is still ADMITTED. ⚠ It does NOT still lower correctly through
    // the patternMatches slot handlers on this surviving path — MEASURED
    // (issue 3899): codegen's own field-type resolution has the identical
    // unsubstituted-type-parameter gap, so it takes the WRONG (non-nullable)
    // comparison branch entirely and never reaches `nv.range.match`/
    // `nv.relop.match` at all. codegen/case_relop_nullable_1074_test.cpp's
    // `generic_record_slot_ordering_pattern_ice_3899` row pins the actual
    // result: a clean, loud "IR verification failed" compile error — not a
    // correct value, and not those handlers being exercised.
    auto r = parseSemaAll(
        "module M; record Box<T>(X: T, Y: int); "
        "class C public static method Main(): int "
        "  var b := new Box<int?>(99, 1); "
        "  return (case b of Box(1..5, 1) => 7, _ => 3); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasNullablePatternError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
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
    // The one place §17.5 and §19.3 deliberately DISAGREE, recorded so the
    // next reader does not file it as a defect. §19.3: "pattern position
    // performs no narrowing" — so a scrutinee whose DECLARED type is nullable
    // is rejected at a range pattern even where §7.4 has proved it non-null,
    // while the same narrowing DOES rescue `n in 1..5` (§7.11's static-type
    // reading, which §17.5 defers to through §5.1). §16.6's parity claim is
    // stated "over a nullable `v`", i.e. about the un-narrowed operand, and is
    // pinned by section_16_6_parity_in_and_case_agree above.
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
    BEZEL_EXPECT_FALSE(narrowedIn.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNullablePatternError(narrowedCase));
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

BEZEL_TEST(lifted_operators_rejected_3298, increment_on_nonnullable_record_with_overload_accepted) {
    // The control: §17.6 admits `x++` on a record that overloads the operator.
    // The nullability test above must not have swallowed that.
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
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §7.4 / §5.3 (v0.286): the establishment predicate must read the VALUE, not
// only its type. §5.3 constant-narrowing retypes a literal to the target type,
// so `var a: byte? := 3;` carried a NULLABLE initializer type and established
// no narrowing, while `var a: int? := 3;` did — §7.11 then answered
// differently for the two at identical shapes, decided by the operand's width.
// ---------------------------------------------------------------------------

BEZEL_TEST(lifted_operators_rejected_3298, literal_initializer_narrows_at_every_width) {
    const char* widths[] = {"byte", "sbyte", "short", "ushort",
                            "int", "long", "double", "float", "decimal"};
    for (const char* w : widths) {
        auto r = parseSemaAll(
            std::string("module M; class C public static method Main(): int "
                        "  var a: ") + w + "? := 3; "
            "  var b := a + a; "
            "  return 0; "
            "end Main; end C;");
        BEZEL_EXPECT_FALSE(hasNullableOpError(r));
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

BEZEL_TEST(lifted_operators_rejected_3298, retained_qq_eq_then_compound) {
    // The signature acceptance case from "Done means" in the issue:
    //   n ??= 0; n += 1;
    // where `n: int?`. The `??=` must still be ACCEPTED (per the issue's
    // structural-exclusion rule for `??=`); the subsequent `+=` IS rejected
    // by §7.11 — the issue's exact wording: "must now reject only at the
    // `+=`". This test pins both halves of that pair: it does NOT reject at
    // the `??=` line (the retained acceptance), and DOES reject the `+=`
    // line. (Lifting §7.7's "after `x ??= expr`, x is narrowed non-null" onto
    // a subsequent statement is out of scope — proposal 0114 doesn't rule
    // it. The compound form on a still-nullable target remains a §7.11
    // reject.)
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  var n: int? := null; "
        "  n ??= 0; "
        "  n += 1; "
        "  return 0; "
        "end Main; end C;");
    // The program has at least one diagnostic: the §6.6 / §7.11 reject on
    // the `n += 1` line. It must NOT have a §6.6 / §7.11 reject on the `??=`
    // line — `??=` is structurally excluded from §7.11.
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
    auto r = parseSemaAll(
        "module M; "
        "record V(n: int) "
        "  public static method operator+(a: V, b: V): V => new V(a.n + b.n); "
        "end V; "
        "class C public method M(a: int, b: V?): V => a + b; "
        "end M; end C;");
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
