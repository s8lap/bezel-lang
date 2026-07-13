// Codegen regression tests for §19.4 / §19.5 — a `when` guard (or arm body)
// that accesses a MEMBER (field or method) of a type-pattern-bound variable
// (`var c: Circle when c.r > 5`). These derive from FINDING [5].
//
// Root cause: the TypeResolution pass never visited a case-arm pattern, so the
// `T` in a `var x: T` binding had no resolved TypeRef. ExprTypecheck therefore
// could not type the bound name, the member access `c.r` resolved to no member
// symbol, and codegen's fieldAddr returned null — which made lowerCaseExpr bail
// out mid-CFG, leaving the guard basic block without a terminator (malformed IR
// → SIGILL in LLVM's BranchProbabilityInfo). A second, adjacent defect: the
// typed binding pattern always matched (`patternMatches` short-circuited every
// BindingPattern to `true`), so `var c: Circle` matched a non-Circle scrutinee
// and a member access on it would read the wrong layout (§19.5 requires a type
// test). Both are fixed here.
//
// §19.4 Guards: "The first arm whose pattern matches *and* whose guard is true
// is selected"; the bound variable is usable in the guard.
// §19.5: a typed binding pattern `var x: T` matches only values that are-a T.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// The Shape/Circle/Square hierarchy from FINDING [5], shared by every test.
const char* kPrologue =
    "module M; "
    "abstract class Shape "
    "  public abstract method Area(): int; "
    "  destructor end destructor; "
    "end Shape; "
    "class Circle : Shape "
    "  public r: int; "
    "  public constructor(x: int) r := x; end constructor; "
    "  public override method Area(): int => r; "
    "  destructor end destructor; "
    "end Circle; "
    "class Square : Shape "
    "  public s: int; "
    "  public constructor(x: int) s := x; end constructor; "
    "  public override method Area(): int => s * s; "
    "  destructor end destructor; "
    "end Square; ";

// Run `kPrologue` + the given `classC` definition.
int run(const std::string& classC) {
    auto r = runAot(std::string(kPrologue) + classC);
    return r.ran ? r.exitCode : -999;
}

// String-channel counterpart (runAotOut) for observables outside 0..255.
std::string runOut(const std::string& classC) {
    return runAotOut(std::string(kPrologue) + classC);
}

} // namespace

// ── FINDING [5]: field-member guard, expression position ──────────────────

// §19.4: `var c: Circle when c.r > 5` over a Circle(10) — the guard reads the
// narrowed field; the guard is true so the arm is selected. (Previously SIGILL.)
BEZEL_TEST(codegen_pattern_guard_member, field_guard_true_expr_pos) {
    BEZEL_EXPECT_EQ(runOut(
        "class C "
        "  public static method K(sh: Shape): int "
        "    return case sh of var c: Circle when c.r > 5 => 1000, _ => 0 end; "
        "  end K; "
        "  public static method Main(): int "
        "    using var c := new Circle(10); Console.WriteLine(K(c)); return 0; "
        "  end Main; "
        "end C;"), "1000");
}

// §19.4: same guard over a Circle(3) — the guard is false, so control falls to
// the `_` arm. Confirms the guard branch wires through to the next arm.
BEZEL_TEST(codegen_pattern_guard_member, field_guard_false_falls_through) {
    BEZEL_EXPECT_EQ(run(
        "class C "
        "  public static method K(sh: Shape): int "
        "    return case sh of var c: Circle when c.r > 5 => 1000, _ => 0 end; "
        "  end K; "
        "  public static method Main(): int "
        "    using var c := new Circle(3); return K(c); "
        "  end Main; "
        "end C;"), 0);
}

// ── FINDING [5]: method-member guard ──────────────────────────────────────

// §19.4 / §17: `var c: Circle when c.Area() > 5` — a method call on the bound
// variable in the guard. (A method call can split blocks; the guard's
// conditional branch must still terminate the resulting block.)
BEZEL_TEST(codegen_pattern_guard_member, method_guard_true) {
    BEZEL_EXPECT_EQ(runOut(
        "class C "
        "  public static method K(sh: Shape): int "
        "    return case sh of var c: Circle when c.Area() > 5 => 1000, _ => 0 end; "
        "  end K; "
        "  public static method Main(): int "
        "    using var c := new Circle(9); Console.WriteLine(K(c)); return 0; "
        "  end Main; "
        "end C;"), "1000");
}

BEZEL_TEST(codegen_pattern_guard_member, method_guard_false_falls_through) {
    BEZEL_EXPECT_EQ(run(
        "class C "
        "  public static method K(sh: Shape): int "
        "    return case sh of var c: Circle when c.Area() > 5 => 1000, _ => 0 end; "
        "  end K; "
        "  public static method Main(): int "
        "    using var c := new Circle(2); return K(c); "
        "  end Main; "
        "end C;"), 0);
}

// ── FINDING [5]: statement-position case with member guards ────────────────

// §19.4 statement position — same field guard, arm bodies are `do … end`.
BEZEL_TEST(codegen_pattern_guard_member, field_guard_statement_pos) {
    BEZEL_EXPECT_EQ(runOut(
        "class C "
        "  public static method K(sh: Shape): int "
        "    var r := 0; "
        "    case sh of "
        "      var c: Circle when c.r > 5 => do r := 1000; end, "
        "      _ => do r := 0; end "
        "    end; "
        "    return r; "
        "  end K; "
        "  public static method Main(): int "
        "    using var c := new Circle(10); Console.WriteLine(K(c)); return 0; "
        "  end Main; "
        "end C;"), "1000");
}

// §19.4 statement position — method guard in stmt-position case.
BEZEL_TEST(codegen_pattern_guard_member, method_guard_statement_pos) {
    BEZEL_EXPECT_EQ(runOut(
        "class C "
        "  public static method K(sh: Shape): int "
        "    var r := 0; "
        "    case sh of "
        "      var c: Circle when c.Area() > 5 => do r := 1000; end, "
        "      _ => do r := 0; end "
        "    end; "
        "    return r; "
        "  end K; "
        "  public static method Main(): int "
        "    using var c := new Circle(9); Console.WriteLine(K(c)); return 0; "
        "  end Main; "
        "end C;"), "1000");
}

// ── FINDING [5]: more complex member expression in the guard ───────────────

// §19.4 / §17: a guard that reads the same member twice in an arithmetic
// expression (`c.r * 2 > c.r`). True for any r > 0.
BEZEL_TEST(codegen_pattern_guard_member, complex_member_expr_guard) {
    BEZEL_EXPECT_EQ(runOut(
        "class C "
        "  public static method K(sh: Shape): int "
        "    return case sh of var c: Circle when c.r * 2 > c.r => 1000, _ => 0 end; "
        "  end K; "
        "  public static method Main(): int "
        "    using var c := new Circle(10); Console.WriteLine(K(c)); return 0; "
        "  end Main; "
        "end C;"), "1000");
}

// ── §19.5: a typed binding pattern is a type test (must not match an
// unrelated type) — soundness for member access on the bound variable ──────

// §19.5: a Square scrutinee must NOT match the `var c: Circle` arm; it falls
// through to the `var sq: Square` arm. (Before the fix, the typed binding
// always matched, so the Square ran the Circle arm.)
BEZEL_TEST(codegen_pattern_guard_member, typed_binding_distinguishes_types) {
    BEZEL_EXPECT_EQ(run(
        "class C "
        "  public static method K(sh: Shape): int "
        "    return case sh of "
        "      var c: Circle => 100, "
        "      var sq: Square => 200, "
        "      _ => 0 "
        "    end; "
        "  end K; "
        "  public static method Main(): int "
        "    using var sq := new Square(4); return K(sq); "
        "  end Main; "
        "end C;"), 200);
}

// §19.5 + §19.4: a Square does not match `var c: Circle when c.r > 5`; the
// guard (which reads a Circle-only field) is never evaluated for the Square,
// which falls through to `_`. Confirms the type test gates the member access.
BEZEL_TEST(codegen_pattern_guard_member, non_matching_type_skips_member_guard) {
    BEZEL_EXPECT_EQ(run(
        "class C "
        "  public static method K(sh: Shape): int "
        "    return case sh of var c: Circle when c.r > 5 => 1000, _ => 7 end; "
        "  end K; "
        "  public static method Main(): int "
        "    using var sq := new Square(99); return K(sq); "
        "  end Main; "
        "end C;"), 7);
}

// ── Controls (must still pass; from FINDING [5]'s isolating set) ───────────

// Control: same arm, NO guard.
BEZEL_TEST(codegen_pattern_guard_member, control_no_guard) {
    BEZEL_EXPECT_EQ(run(
        "class C "
        "  public static method K(sh: Shape): int "
        "    return case sh of var c: Circle => 100, _ => 0 end; "
        "  end K; "
        "  public static method Main(): int "
        "    using var c := new Circle(10); return K(c); "
        "  end Main; "
        "end C;"), 100);
}

// Control: constant guard (`when true`).
BEZEL_TEST(codegen_pattern_guard_member, control_constant_guard) {
    BEZEL_EXPECT_EQ(run(
        "class C "
        "  public static method K(sh: Shape): int "
        "    return case sh of var c: Circle when true => 77, _ => 0 end; "
        "  end K; "
        "  public static method Main(): int "
        "    using var c := new Circle(10); return K(c); "
        "  end Main; "
        "end C;"), 77);
}

// Control: guard passes the WHOLE bound var to a static method (no narrowed
// member access in the guard).
BEZEL_TEST(codegen_pattern_guard_member, control_whole_var_to_function) {
    BEZEL_EXPECT_EQ(run(
        "class C "
        "  public static method Big(c: Circle): bool => c.r > 5; "
        "  public static method K(sh: Shape): int "
        "    return case sh of var c: Circle when Big(c) => 77, _ => 0 end; "
        "  end K; "
        "  public static method Main(): int "
        "    using var c := new Circle(10); return K(c); "
        "  end Main; "
        "end C;"), 77);
}

// Control: guard over an OUTER variable (not the bound one).
BEZEL_TEST(codegen_pattern_guard_member, control_outer_var_guard) {
    BEZEL_EXPECT_EQ(run(
        "class C "
        "  public static method K(sh: Shape, threshold: int): int "
        "    return case sh of var c: Circle when threshold > 5 => 77, _ => 0 end; "
        "  end K; "
        "  public static method Main(): int "
        "    using var c := new Circle(10); return K(c, 9); "
        "  end Main; "
        "end C;"), 77);
}

// Control: bound-var member access in the arm BODY (not the guard).
BEZEL_TEST(codegen_pattern_guard_member, control_member_access_in_body) {
    BEZEL_EXPECT_EQ(run(
        "class C "
        "  public static method K(sh: Shape): int "
        "    return case sh of var c: Circle => c.r, _ => 0 end; "
        "  end K; "
        "  public static method Main(): int "
        "    using var c := new Circle(8); return K(c); "
        "  end Main; "
        "end C;"), 8);
}
