// Spec-derived codegen tests for fix-forward #5173 — §19.3 / §7.11 / §10
// (v0.310, #5143 ruling): a §7.4-narrowed non-null read of a nullable
// ENUM scrutinee with a relational/range pattern is ADMITTED at sema AND
// lowers to a correct ordinal comparison at codegen (the §10 enum lowers
// to i32 — CG44 — and patternMatches's `isNullableVal` branch extracts
// the unwrapped i32 payload from the `{i8 hasValue; i32}` box via
// `nullablePrimValue`).
//
// CONTROL tests for the predicate tightening at src/sema/ExprTypecheck.cpp:
// 13036–13062. The narrowing path opens for value-typed scrutinees
// (Primitive+value / Unit / InlineArray / Named+Record+Enum+Choice);
// the fix-forward excludes Primitive's reference members (String, Object,
// None) from the value arm. enum E? is in the Named+Enum arm — pre- and
// post-fix-forward ADMITTED, and the codegen path lowers correctly. These
// rows prove the predicate tightening does NOT accidentally over-restrict
// the enum admit.
//
// runAot drives the real AOT path (fork-isolated Driver::compileFile + clang
// link + native exec) — the strict oracle. Each row asserts the SELECTED
// arm's value on stdout, so a regression in the enum's lower (pointer-compare
// shape, missing isNullableVal branch, etc.) flips a test.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// §19.3 / §10 (v0.310, #5143 ruling): a §7.4-narrowed non-null read of a
// nullable ENUM scrutinee (`E?`) at a relational pattern is admitted at
// sema and lowers via patternMatches's `isNullableVal` path
// (isNullableEnumTy → nullablePrimValue extracts the unwrapped i32
// underlying type from the {i8 hasValue; i32} box). The selected arm's
// value is observed in the exit code.
//
// Helper.Get(): T? returns the nullable form so `valueProvesNonNull` is
// false and the local is NOT narrowed at the assignment; the
// `if x <> null then case x of …` block performs the §7.4 narrowing for
// the case form (§19.1). Same Main/Helper structure the existing
// case_relop_nullable_1074 rows use (#5173's documentation).
struct RunResult {
    bool ran;
    int exitCode;
};
RunResult runProg(const std::string& body) {
    auto r = runAot(body);
    return {r.ran, r.exitCode};
}

}  // namespace

// §19.3 / §10 / #5173: enum E? narrowed — `1` matches `> E.A` (where E.A
// is ordinal 0, E.B is ordinal 1, E.C is ordinal 2) and selects arm 1.
BEZEL_TEST(scrutinee_enum_narrowed_relop_5173,
           narrowed_enum_scrutinee_gt_admitted_5173) {
    auto r = runProg(
        "module M; "
        "enum E A, B, C; end E; "
        "class Helper public static method Get(): E? "
        "  return E.B; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: E? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of > E.A => 1, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §19.3 / §10: enum E? narrowed, `E.A` does NOT match `> E.A` and selects
// the `_ => 2` arm.
BEZEL_TEST(scrutinee_enum_narrowed_relop_5173,
           narrowed_enum_scrutinee_gt_no_match_admitted_5173) {
    auto r = runProg(
        "module M; "
        "enum E A, B, C; end E; "
        "class Helper public static method Get(): E? "
        "  return E.A; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: E? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of > E.A => 1, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// §19.3 / §10: enum E? narrowed — `E.B` (ordinal 1) matches `<= E.B`
// (boundary inclusive) and selects arm 8.
BEZEL_TEST(scrutinee_enum_narrowed_relop_5173,
           narrowed_enum_scrutinee_le_boundary_admitted_5173) {
    auto r = runProg(
        "module M; "
        "enum E A, B, C; end E; "
        "class Helper public static method Get(): E? "
        "  return E.B; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: E? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of <= E.B => 8, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);
}

// §19.3 / §10: `<` and `>=` spellings — `E.A` (ordinal 0) matches `< E.B`
// (ordinal 1); `E.C` (ordinal 2) matches `>= E.C`.
BEZEL_TEST(scrutinee_enum_narrowed_relop_5173,
           narrowed_enum_scrutinee_lt_and_ge_admitted_5173) {
    auto r1 = runProg(
        "module M; "
        "enum E A, B, C; end E; "
        "class Helper public static method Get(): E? "
        "  return E.A; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: E? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of < E.B => 4, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r1.ran);
    BEZEL_EXPECT_EQ(r1.exitCode, 4);

    auto r2 = runProg(
        "module M; "
        "enum E A, B, C; end E; "
        "class Helper public static method Get(): E? "
        "  return E.C; end Get; end Helper; "
        "class C public static method Main(): int "
        "  var n: E? := Helper.Get(); "
        "  if n <> null then "
        "    return (case n of >= E.C => 6, _ => 2); "
        "  end if; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r2.ran);
    BEZEL_EXPECT_EQ(r2.exitCode, 6);
}

// (Range patterns over enum scrutinees — `E.A..E.C` — are not part of the
// #5173 spec coverage: the existing parser/codegen path emits "expected
// '=>' after case-arm pattern" for an enum-bounded RangePattern, so the
// range form is excluded from this control file. The relational patterns
// above exercise the same `isNullableVal` branch — `isNullableEnumTy` is
// the enum analogue of `isNullablePrimTy` per §10 / CG44, and patternMatches
// unwraps the {i8 hasValue; i32} box via `nullablePrimValue` exactly as it
// does for a primitive. Range-pattern coverage for a NARROWED ENUM
// scrutinee stays a residual — separate from #5173 (filed separately).)