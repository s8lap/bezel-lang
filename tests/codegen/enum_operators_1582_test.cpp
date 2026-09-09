// Spec-derived codegen tests for issue #1582 — enum operator overloads:
// parse, sema-check, and runtime dispatch/lowering (§10.4, §25/§25.2, grammar
// A.3/A.4; v0.183, #1381).
//
// §10.4: "An enum may declare ... operator overloads (§25; grammar A.3)."
// §25: "Records, enums, and classes may overload operators." §25's resolution
// rule (§25.2) draws the candidate set from `operator <op>` overloads
// declared on the operands' static types — an enum operand's own declared
// operators must be found and dispatched exactly as a record's are.
//
// Each test forces the user overload to compute something DIFFERENT from
// what the built-in-on-the-underlying-integer behavior (or no overload at
// all) would produce, so a passing test proves dispatch actually reached the
// user's declaration rather than some other codegen path.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// §10.4/§25.3: binary operator, expression body — heterogeneous operands
// (`Priority + int`, declaring type in the first position per §25.2's
// declaring-type-operand rule). `Priority.Low + 2` → `High` → int cast 2.
BEZEL_TEST(codegen_enum_operators_1582, binary_plus_expression_body_spec_10_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Priority Low, Medium, High; "
        "  public static method operator +(a: Priority, b: int): Priority "
        "    => Priority(int(a) + b); "
        "end Priority; "
        "class C public static method Main(): int "
        "  var p: Priority := Priority.Low; "
        "  var q: Priority := p + 2; "
        "  return int(q); "
        "end Main; end C;"), 2);
}

// §25.3: block-bodied operator with the named-end form `end operator +;`.
BEZEL_TEST(codegen_enum_operators_1582, binary_plus_block_body_named_end_spec_25_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Priority Low, Medium, High; "
        "  public static method operator +(a: Priority, b: int): Priority "
        "    return Priority(int(a) + b); "
        "  end operator +; "
        "end Priority; "
        "class C public static method Main(): int "
        "  return int(Priority.Low + 1); "
        "end Main; end C;"), 1);
}

// §10.4/§25.3: unary operator overload on an enum with explicit underlying
// values (§10.2) — `-Sign.Pos` → `Sign.Neg` → int cast -1, returned as the
// unsigned exit-code bit pattern the harness compares against.
BEZEL_TEST(codegen_enum_operators_1582, unary_minus_spec_10_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Sign: int Neg := -1, Zero := 0, Pos := 1; "
        "  public static method operator -(a: Sign): Sign => Sign(-int(a)); "
        "end Sign; "
        "class C public static method Main(): int "
        "  var s: Sign := Sign.Pos; "
        "  var t: Sign := -s; "
        "  return (if t = Sign.Neg then 1 else 0); "
        "end Main; end C;"), 1);
}

// §25.2: comparison operators must be a complete group; here the group is
// declared with REVERSED semantics (`<` implemented as the underlying `>`)
// so a passing test proves `Low < High` dispatches to the user overload
// (evaluates to false) rather than the underlying-integer default (which
// would evaluate to true).
BEZEL_TEST(codegen_enum_operators_1582,
           comparison_group_dispatches_to_declared_overload_spec_25_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Priority Low, Medium, High; "
        "  public static method operator <(a: Priority, b: Priority): bool => int(a) > int(b); "
        "  public static method operator >(a: Priority, b: Priority): bool => int(a) < int(b); "
        "  public static method operator <=(a: Priority, b: Priority): bool => int(a) >= int(b); "
        "  public static method operator >=(a: Priority, b: Priority): bool => int(a) <= int(b); "
        "end Priority; "
        "class C public static method Main(): int "
        "  return (if Priority.Low < Priority.High then 1 else 0); "
        "end Main; end C;"), 0);
}

// §25.2 co-declaration (§25.2 v0.158, enum spelling v0.183): `=`/`<>`
// overloaded with a plain (non-`override`, per §10.5) co-declared
// `Equals(T)`/`GetHashCode`. The overload treats every pair as equal
// (opposite of the underlying-integer default), so a passing test proves
// `a = b` dispatches to the declared operator.
BEZEL_TEST(codegen_enum_operators_1582,
           equality_group_dispatches_to_declared_overload_spec_25_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Priority Low, Medium, High; "
        "  public static method operator =(a: Priority, b: Priority): bool => true; "
        "  public static method operator <>(a: Priority, b: Priority): bool => false; "
        "  public method Equals(other: Priority): bool => true; "
        "  public method GetHashCode(): int => 1; "
        "end Priority; "
        "class C public static method Main(): int "
        "  return (if Priority.Low = Priority.High then 1 else 0); "
        "end Main; end C;"), 1);
}

// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix comment (kept for
// history): "`p++` on an enum dispatches to `operator ++` (postfix-only,
// statement-only, §25.2) and writes the result back — two increments from
// `Low` (0) land on `High` (2)." §25.1 (v0.295), verbatim, in the *not
// overloadable* list: "The increment and decrement statements `++` and
// `--` — built-in statements over the operand types §6.6's compound form
// admits (§17.6), not operators of the expression grammar." The
// declaration itself is now a compile error (TypeResolution_Checks.cpp's
// `checkOperatorGroups`, which already rejects `not`/`!`/`??`/... by the
// same mechanism) — this row now pins the rejection instead of the
// dispatch value.
BEZEL_TEST(codegen_enum_operators_1582,
           postfix_increment_declaration_rejected_spec_25_1_issue_3978) {
    auto r = runAot(
        "module M; "
        "enum Priority Low, Medium, High; "
        "  public static method operator ++(p: Priority): Priority => Priority(int(p) + 1); "
        "end Priority; "
        "class C public static method Main(): int "
        "  var p: Priority := Priority.Low; "
        "  p++; p++; "
        "  return int(p); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    bool citedDecl = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("operator `++` may not be overloaded (§25.1)") !=
            std::string::npos)
            citedDecl = true;
    BEZEL_EXPECT_TRUE(citedDecl);
}

// §10.4: an enum operator overload coexists with an ordinary instance method
// declared in the same body (§10.4's own `IsUrgent` example) — both resolve
// and compute independently of one another.
BEZEL_TEST(codegen_enum_operators_1582, operator_coexists_with_instance_method_spec_10_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Priority Low, Medium, High; "
        "  public static method operator +(a: Priority, b: int): Priority "
        "    => Priority(int(a) + b); "
        "  public method IsUrgent(): bool => this = High; "
        "end Priority; "
        "class C public static method Main(): int "
        "  var p: Priority := Priority.Low; "
        "  var q: Priority := p + 2; "
        "  return (if q.IsUrgent() then 1 else 0); "
        "end Main; end C;"), 1);
}
