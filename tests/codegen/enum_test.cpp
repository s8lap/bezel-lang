// Codegen slice CG30 — enums (§10). An enum is a value type backed by an
// integer; each constant lowers to its computed value (explicit `:= N`,
// else previous + 1 from 0). Enum-typed locals are integers, and enum
// equality is an integer comparison. Observable via the int exit code.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

int run(const std::string& enumDecl, const std::string& body) {
    auto r = runAot(
        "module M; " + enumDecl +
        " class C public static method Main(): int " + body +
        " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

const char* kColor = "enum Color Red := 1, Green := 2, Blue := 3; end Color;";

} // namespace

BEZEL_TEST(codegen_cg30, constant_equality) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c := Color.Green; return (if c = Color.Green then 7 else 0);"), 7);
}

BEZEL_TEST(codegen_cg30, distinct_constants_not_equal) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c := Color.Red; return (if c = Color.Green then 1 else 9);"), 9);
}

BEZEL_TEST(codegen_cg30, inequality_operator) {
    BEZEL_EXPECT_EQ(run(kColor,
        "return (if Color.Red <> Color.Blue then 4 else 0);"), 4);
}

BEZEL_TEST(codegen_cg30, reassignment) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c := Color.Red; c := Color.Blue; "
        "return (if c = Color.Blue then 3 else 0);"), 3);
}

BEZEL_TEST(codegen_cg30, used_in_if_condition) {
    BEZEL_EXPECT_EQ(run(kColor,
        "var c := Color.Blue; "
        "if c = Color.Blue then return 11; end; return 0;"), 11);
}

BEZEL_TEST(codegen_cg30, implicit_auto_increment_values) {
    // A, B, C with no explicit values are 0, 1, 2 — so A <> C and they
    // round-trip through a local.
    BEZEL_EXPECT_EQ(run(
        "enum E A, B, C; end E;",
        "var x := E.C; return (if x = E.C then 8 else 0);"), 8);
}

BEZEL_TEST(codegen_cg30, explicit_then_implicit_values) {
    // A := 5, B (implicit → 6): B and A are distinct, B equals itself.
    BEZEL_EXPECT_EQ(run(
        "enum E A := 5, B, C; end E;",
        "var x := E.B; "
        "return (if (x = E.B) and (x <> E.A) then 6 else 0);"), 6);
}

// ---- #435 / §10 / §8.6 / §13: enum implementing interface, box thunk --------
// When an enum implements an interface the box thunk must LOAD the i32 enum
// value from the box's value slot (offset 16) and pass it to the enum method
// by value.  The old code GEP'd with an i32 base, which is malformed IR
// (IR verifier failure).  These tests are the repro and regression guard.

BEZEL_TEST(codegen_cg30_iface, enum_via_interface_returns_value) {
    // §10 / §13 / #435: Color : ICode; Code() returns int(this).
    // Dispatch through ICode-typed parameter must yield the enum's i32 value.
    // This is the primary #435 repro: before the fix the IR verifier rejects
    // the module (GEP on i32 base) and the program exits with garbage / 0.
    BEZEL_EXPECT_EQ(runAot(
        "module M; "
        "interface ICode method Code(): int; end ICode; "
        "enum Color : ICode "
        "  Red := 1, Green := 2, Blue := 3; "
        "  public method Code(): int => int(this); "
        "end Color; "
        "class C "
        "  public static method Get(c: ICode): int => c.Code(); "
        "  public static method Main(): int "
        "    var g := Color.Green; return Get(g); "
        "  end Main; "
        "end C;").exitCode, 2);
}

BEZEL_TEST(codegen_cg30_iface, enum_via_interface_two_constants) {
    // §10 / §13 / #435: dispatch through the interface for two different
    // enum constants must return their respective underlying values.
    BEZEL_EXPECT_EQ(runAot(
        "module M; "
        "interface ICode method Code(): int; end ICode; "
        "enum Color : ICode "
        "  Red := 10, Green := 20, Blue := 30; "
        "  public method Code(): int => int(this); "
        "end Color; "
        "class C "
        "  public static method Get(c: ICode): int => c.Code(); "
        "  public static method Main(): int "
        "    var r := Color.Red; var b := Color.Blue; "
        "    return Get(r) + Get(b); "
        "  end Main; "
        "end C;").exitCode, 40);
}

BEZEL_TEST(codegen_cg30_iface, enum_via_interface_with_extra_arg) {
    // §10 / §13 / #435: the thunk must forward extra arguments past `this`
    // unchanged.  Method takes an int `n` and returns int(this) + n.
    BEZEL_EXPECT_EQ(runAot(
        "module M; "
        "interface IAdder method Add(n: int): int; end IAdder; "
        "enum E : IAdder "
        "  A := 5, B := 10; "
        "  public method Add(n: int): int => int(this) + n; "
        "end E; "
        "class C "
        "  public static method Use(a: IAdder): int => a.Add(7); "
        "  public static method Main(): int "
        "    var e := E.B; return Use(e); "
        "  end Main; "
        "end C;").exitCode, 17);
}

BEZEL_TEST(codegen_cg30_iface, record_interface_box_thunk_regression) {
    // §8.6 / §9.6 / §13: REGRESSION — record box thunk must be unaffected
    // by the enum fix.  Rect : IArea; dispatch via IArea must still reach
    // the record's method with the correct field values.
    BEZEL_EXPECT_EQ(runAot(
        "module M; "
        "interface IArea method Area(): int; end IArea; "
        "record Rect(w: int, h: int): IArea "
        "  public method Area(): int => w * h; "
        "end Rect; "
        "class C "
        "  public static method Measure(s: IArea): int => s.Area(); "
        "  public static method Main(): int "
        "    var r := new Rect(6, 7); return Measure(r); "
        "  end Main; "
        "end C;").exitCode, 42);
}

// #739 / §10.2: an explicit NEGATIVE enum value (`B := -5`) must be honored.
// Previously the value computer only handled a bare IntLiteralExpr, so the
// unary-minus `-5` was dropped and B got the auto-increment value (6).
BEZEL_TEST(codegen_enum, negative_explicit_value_739) {
    BEZEL_EXPECT_EQ(run(
        "enum E A := 5, B := -5; end E; ",
        "return (if int(E.B) = -5 then 0 else int(E.B));"), 0);
}

// #739 / §10.2: explicit value as ARITHMETIC and a reference to an earlier
// constant; auto-increment continues from the last explicit value.
BEZEL_TEST(codegen_enum, arithmetic_and_sibling_value_739) {
    BEZEL_EXPECT_EQ(run(
        "enum E A := 10, B, C := A + 100, D := -3; end E; ",
        // A=10, B=11 (auto), C=110, D=-3 → 10+11+110-(-3) = 134
        "return int(E.A) + int(E.B) + int(E.C) - int(E.D);"), 134);
}

// fix #5402 (§10.2, v0.315): a qualified sibling `E.A` in an enum value
// initializer denotes that sibling's underlying value. Before the fix,
// MemberAccessExpr was not handled by foldIntConst, so `E.A` fell back to
// auto-increment and silently produced the wrong value.  Test values are
// chosen so auto-increment (base) gives a DIFFERENT sum than the folded
// computed values (fixed).
BEZEL_TEST(codegen_enum, qualified_sibling_value_5402) {
    // A=10, B=E.A+5=15, C=E.B+5=20
    // Base (no MemberAccessExpr fold): A=10, B=11 (auto), C=12 (auto) → 33
    // Fixed (MemberAccessExpr folds): A=10, B=15, C=20 → 45
    BEZEL_EXPECT_EQ(run(
        "enum E A := 10, B := E.A + 5, C := E.B + 5; end E; ",
        "return int(E.A) + int(E.B) + int(E.C);"),
        45);
}

// fix #5402 (§10.2, v0.315): a SELF-reference in an enum value initializer
// is a compile error. Before the fix, the self-reference was not detected
// and the value silently fell back to auto-increment.
BEZEL_TEST(codegen_enum, self_reference_rejected_5402) {
    auto r = runAot(
        "module M; enum E A := A + 1; end E; class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// fix #5402 (§10.2, v0.315): a FORWARD-reference in an enum value initializer
// is a compile error. Before the fix, the forward-reference was not detected
// and the value silently fell back to auto-increment.
BEZEL_TEST(codegen_enum, forward_reference_rejected_5402) {
    auto r = runAot(
        "module M; enum E C := B + 1, B := 10; end E; class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
