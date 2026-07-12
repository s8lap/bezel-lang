// §19 / §A.8 (#957): a NAMED CONST used as a case pattern.
//
// `case x C.K => …` parses the qualified name `C.K` as a NamedPattern (the
// parser emits a NamedPattern for any non-primitive name and defers
// Constant-vs-Type-test to resolution). Before the fix, codegen's NamedPattern
// handler resolved only enum constants and class/interface type-test names; a
// name that resolves to a named CONST fell through to `return nullptr`, leaving
// `case.end` unterminated → malformed-IR (IR_FAIL) in expression position and a
// matches-nothing case in statement position.
//
// THE FIX: a NamedPattern whose qualified name resolves to a named const folds
// the const's value and compares it against the scrutinee (mirrors the
// ConstantPattern handler — width-reconciled for non-string scrutinees,
// content-compared for strings). Sema also typechecks ConstantPattern constants
// in scrutinee-bearing cases (the `,`-alternative form `C.K, 99`). §19 / §A.8.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// Qualified named const `C.K` in EXPRESSION position — matches (x == C.K).
BEZEL_TEST(case_named_const_pattern_957, qualified_const_expr_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public const K: int := 42; "
        "  public static method Main(): int "
        "    var x := 42; "
        "    return case x of  C.K => 0, _ => 1 end; "  // 0
        "  end Main; "
        "end C;"), 0);
}

// Qualified named const in expression position — does NOT match → wildcard.
BEZEL_TEST(case_named_const_pattern_957, qualified_const_expr_nomatch) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public const K: int := 42; "
        "  public static method Main(): int "
        "    var x := 7; "
        "    return case x of  C.K => 0, _ => 1 end; "  // 1
        "  end Main; "
        "end C;"), 1);
}

// Qualified named const in STATEMENT position — matches.
BEZEL_TEST(case_named_const_pattern_957, qualified_const_stmt_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public const K: int := 42; "
        "  public static method Main(): int "
        "    var x := 42; "
        "    var r := 9; "
        "    case x of  C.K => r := 0;, _ => r := 1; end; "
        "    return r; "  // 0
        "  end Main; "
        "end C;"), 0);
}

// String-typed named const — content comparison (match and non-match).
BEZEL_TEST(case_named_const_pattern_957, string_const_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public const S: string := \"hi\"; "
        "  public static method Main(): int "
        "    var x := \"hi\"; "
        "    return case x of  C.S => 5, _ => 6 end; "  // 5
        "  end Main; "
        "end C;"), 5);
}

BEZEL_TEST(case_named_const_pattern_957, string_const_nomatch) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public const S: string := \"hi\"; "
        "  public static method Main(): int "
        "    var x := \"bye\"; "
        "    return case x of  C.S => 5, _ => 6 end; "  // 6
        "  end Main; "
        "end C;"), 6);
}

// A bare own-class const `K` (unqualified) resolves to a unique const.
BEZEL_TEST(case_named_const_pattern_957, bare_own_class_const_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public const K: int := 3; "
        "  public static method Main(): int "
        "    var x := 3; "
        "    return case x of  K => 7, _ => 8 end; "  // 7
        "  end Main; "
        "end C;"), 7);
}

// A `,`-alternative pattern that mixes a named const and a literal: the sema
// fix typechecks the ConstantPattern's constants in a scrutinee-bearing case.
BEZEL_TEST(case_named_const_pattern_957, const_alternative_with_literal) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public const K: int := 42; "
        "  public static method Main(): int "
        "    var x := 99; "
        "    return case x of  C.K | 99 => 0, _ => 1 end; "  // 0
        "  end Main; "
        "end C;"), 0);
}

// Regression guard: an enum-constant NamedPattern over an enum scrutinee still
// resolves by name against the scrutinee enum (NOT diverted by the const path —
// an enum constant is an EnumConstantSymbol, not a ConstMemberSymbol).
BEZEL_TEST(case_named_const_pattern_957, enum_constant_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public static method Main(): int "
        "    var c := Color.Green; "
        "    return case c of  Red => 1, Green => 2, Color.Blue => 3 end; "  // 2
        "  end Main; "
        "end C;"), 2);
}
