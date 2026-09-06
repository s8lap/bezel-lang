// Regression tests for #1096 (§25): a binary operator overload whose operands
// did not match its declared parameter ORDER (e.g. `4 * v` against
// `operator *(Vec, int)`) was accepted by sema (matched on the right operand's
// type) but emitted args in source order at codegen → an IR-verification crash.
// Bezel does not auto-commute operator overloads (§25 defines none, and a swap
// would be unsound for non-commutative operators like `-`), so the operands
// must match the overload's parameter order; a mismatch is now a clean §25
// compile error instead of a crash. Found by adversarial discovery sweep #7.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/aot_result_asserts.hpp"   // fix #3895: failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
const char* VEC =
    "module M; "
    "record Vec(X: int, Y: int) "
    "  public static method operator *(v: Vec, scalar: int): Vec => "
    "    new Vec(v.X * scalar, v.Y * scalar); "
    "  public static method operator +(a: Vec, b: Vec): Vec => "
    "    new Vec(a.X + b.X, a.Y + b.Y); "
    "end Vec; ";
}  // namespace

// #1096: a reversed-operand use (`4 * v` with only `operator *(Vec, int)`) is a
// clean §25 compile error — NOT an IR-verification crash.
BEZEL_TEST(operator_operand_order_1096, reversed_operands_rejected_not_crash) {
    auto r = runAot(
        std::string(VEC) +
        "class Mn public static method Main(): int "
        "  var v := new Vec(2, 3); "
        "  var r := 4 * v; "                              // (int, Vec) ≠ (Vec, int)
        "  return r.X; "
        "end Main; end Mn;");
    // fix #3895 (issue #3892): the row's own name says "not crash" — a bare
    // `!ran`/`hasErrors()` pair is exactly what would pass if it DID.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// The correctly-ordered use compiles and computes.
BEZEL_TEST(operator_operand_order_1096, correct_order_works) {
    BEZEL_EXPECT_EQ(run(
        std::string(VEC) +
        "class Mn public static method Main(): int "
        "  var v := new Vec(2, 3); "
        "  var r := v * 4; "                              // (Vec, int) → (8, 12)
        "  return r.X; "
        "end Main; end Mn;"), 8);
}

// A same-type operator (record + record) is unaffected.
BEZEL_TEST(operator_operand_order_1096, record_plus_record_works) {
    BEZEL_EXPECT_EQ(run(
        std::string(VEC) +
        "class Mn public static method Main(): int "
        "  var r := new Vec(2, 3) + new Vec(10, 20); "    // (12, 23)
        "  return r.X; "
        "end Main; end Mn;"), 12);
}

// An undeclared operator on a record operand is a clean error (no crash).
BEZEL_TEST(operator_operand_order_1096, undeclared_operator_rejected) {
    auto r = runAot(
        std::string(VEC) +
        "class Mn public static method Main(): int "
        "  var v := new Vec(2, 3); "
        "  var r := v - new Vec(1, 1); "                  // no operator -
        "  return r.X; "
        "end Main; end Mn;");
    // fix #3895 (issue #3892): the row's own name says "not crash" — a bare
    // `!ran`/`hasErrors()` pair is exactly what would pass if it DID.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// Regression guard: built-in arithmetic on primitives (and string `+`) is
// unaffected by the record/class operator reject.
BEZEL_TEST(operator_operand_order_1096, builtin_arithmetic_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"ab\" + \"cd\"; "
        "  var d: decimal := 1.5m + 2.5m; "
        "  return s.Length + int(d); "                    // 4 + 4 = 8
        "end Main; end C;"), 8);
}

// ── #1131 / §22.6 / §25: ALL overloads of an operator symbol are reachable ────
// A type may declare several overloads of the same operator; resolution must
// pick the one matching the operands, not just the first-declared.

// `V + V` resolves the SECOND-declared `operator +(V, V)` overload (the first is
// `operator +(V, int)`), which was previously dead.
BEZEL_TEST(operator_operand_order_1096, second_overload_reachable_1131) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record V(N: int) "
        "  public static method operator +(a: V, b: int): V => new V(a.N + b); "
        "  public static method operator +(a: V, b: V): V => new V(a.N + b.N); "
        "end V; "
        "class App public static method Main(): int "
        "  var a := new V(10); var b := new V(5); "
        "  return (a + b).N; "                             // 15 (V+V overload)
        "end Main; end App;"), 15);
}

// The first-declared overload (`V + int`) still resolves correctly.
BEZEL_TEST(operator_operand_order_1096, first_overload_still_resolves_1131) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record V(N: int) "
        "  public static method operator +(a: V, b: int): V => new V(a.N + b); "
        "  public static method operator +(a: V, b: V): V => new V(a.N + b.N); "
        "end V; "
        "class App public static method Main(): int "
        "  var a := new V(10); "
        "  return (a + 7).N; "                             // 17 (V+int overload)
        "end Main; end App;"), 17);
}

// ── #1132 / §7.11: a user operator overload over a NULLABLE operand is a clean ─
// reject (lifting of user-defined operators over T? is deferred), not malformed
// IR. The operator previously resolved (nullability was ignored) and codegen
// passed the {i8, Vec} wrapper struct to the bare-Vec operator function.
BEZEL_TEST(operator_operand_order_1096, nullable_user_operator_rejected_not_crash_1132) {
    auto r = runAot(
        std::string(VEC) +
        "class Mn public static method Main(): int "
        "  var a: Vec? := new Vec(1, 2); "
        "  var c := a + a; "                               // Vec? + Vec? — deferred
        "  return 0; "
        "end Main; end Mn;");
    // fix #3895 (issue #3892): the row's own name says "not crash" — a bare
    // `!ran`/`hasErrors()` pair is exactly what would pass if it DID.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// Unwrapping the nullable operands with `!` resolves to the non-nullable
// operator and computes correctly (the spec-endorsed workaround).
BEZEL_TEST(operator_operand_order_1096, nullable_user_operator_unwrap_works_1132) {
    BEZEL_EXPECT_EQ(run(
        std::string(VEC) +
        "class Mn public static method Main(): int "
        "  var a: Vec? := new Vec(1, 2); "
        "  var b: Vec? := new Vec(3, 4); "
        "  var c := a! + b!; "
        "  return c.X + c.Y; "                             // 4 + 6 = 10
        "end Main; end Mn;"), 10);
}

// Non-nullable record operator use is unaffected (regression guard).
BEZEL_TEST(operator_operand_order_1096, non_nullable_record_operator_unaffected_1132) {
    BEZEL_EXPECT_EQ(run(
        std::string(VEC) +
        "class Mn public static method Main(): int "
        "  var a := new Vec(1, 2); var b := new Vec(3, 4); "
        "  var c := a + b; "
        "  return c.X + c.Y; "                             // 10
        "end Main; end Mn;"), 10);
}

// #1253 (§14/§25.3): a GENERIC method whose body invokes a user-defined record
// operator must not crash the compiler. emitMethodCall dereferenced `md` (the
// operator's ast decl, which is NOT an ast::MethodDecl → null) unguarded while
// deciding whether to force-specialize. Now null-guarded. Add<int>(Vec(3,4),
// Vec(7,6)) => Vec(10,10) => 20.
BEZEL_TEST(operator_operand_order_1096, record_operator_in_generic_method_no_crash_1253) {
    BEZEL_EXPECT_EQ(run(
        std::string(VEC) +
        "class C "
        "  public static method Add<T>(a: Vec, b: Vec): Vec => a + b; "
        "  public static method Main(): int "
        "    var r := Add<int>(new Vec(3, 4), new Vec(7, 6)); "
        "    return r.X + r.Y; "
        "end Main; end C;"), 20);
}
