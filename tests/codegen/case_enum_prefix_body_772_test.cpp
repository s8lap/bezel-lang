// fix #772 / §10.6: inside a `case` whose scrutinee is an enum, the enum prefix
// may be omitted THROUGHOUT the construct — including arm BODIES, not only arm
// patterns.
//
// §10.6: "Inside a `case` whose scrutinee is an enum, the enum prefix may be
// omitted." The compiler resolved bare member names in arm patterns but not in
// arm body expressions (`Red => Green`), reporting "unresolved identifier
// `Green`" — a FALSE_REJECT. The fix injects the scrutinee enum's constants into
// a scope that parents the arm bodies (for both the expression-position CaseExpr
// and the statement-position CaseStmt), and types a bare enum constant as its
// enum so the arm common type (§20) resolves.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
bool rejected(const std::string& program) {
    return !runJit(program).ran;  // a compile error → never runs
}
}  // namespace

// §10.6: a CaseExpr arm body yields a bare enum constant. Toggle(Red) -> Green,
// Toggle(Green) -> Blue, Toggle(Blue) -> Red. (The #772 repro.)
BEZEL_TEST(case_enum_prefix_body_772, case_expr_bare_enum_body) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public static method Toggle(c: Color): Color => case c "
        "    Red => Green, "
        "    Green => Blue, "
        "    Blue => Red "
        "  end; "
        "  public static method Main(): int "
        "    return 0 if Toggle(Color.Red) = Color.Green else 1; "
        "  end Main; "
        "end C;"), 0);
}

// §10.6: the SAME omission holds in a statement-position CaseStmt arm body.
BEZEL_TEST(case_enum_prefix_body_772, case_stmt_bare_enum_body) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var c := Color.Red; "
        "  var picked := Color.Blue; "
        "  case c "
        "    Red => do picked := Green; end, "
        "    Green => do picked := Blue; end, "
        "    Blue => do picked := Red; end "
        "  end; "
        "  return 0 if picked = Color.Green else 1; "
        "end Main; end C;"), 0);
}

// §10.6: a binding-pattern arm body also resolves bare enum names (the binding
// scope chains to the injected enum scope).
BEZEL_TEST(case_enum_prefix_body_772, binding_arm_body_resolves_bare_enum) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var c := Color.Blue; "
        "  var r := case c "
        "    Red => Green, "
        "    var other => other "  // binds the value; body unaffected
        "  end; "
        "  return 0 if r = Color.Blue else 1; "
        "end Main; end C;"), 0);
}

// §10.6 guard: a genuinely-unresolved name in a case arm body is STILL rejected
// (the injection only adds the scrutinee enum's constants, not arbitrary names).
BEZEL_TEST(case_enum_prefix_body_772, unknown_name_in_arm_body_still_rejected) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var c := Color.Red; "
        "  return case c Red => Nonexistent, Green => 2, Blue => 3 end; "
        "end Main; end C;"));
}

// §10.6 guard: the omission is scoped to the case construct — a bare enum name
// OUTSIDE any case (no enum scrutinee in scope) is STILL unresolved.
BEZEL_TEST(case_enum_prefix_body_772, bare_enum_outside_case_still_rejected) {
    BEZEL_EXPECT_TRUE(rejected(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var x := Green; "
        "  return 0; "
        "end Main; end C;"));
}
