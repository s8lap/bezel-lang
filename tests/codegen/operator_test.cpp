// Codegen slice CG48 — user-defined operator overloading (§25). A binary
// expression on a class/record type that declares a matching `static method
// operator <op>` resolves to that operator method and calls it. Operators
// are emitted via the shared callable-shape path (they carry an
// `OperatorDecl` rather than a `MethodDecl`). Observable via exit code.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

bool diagContainsOp(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}
} // namespace

BEZEL_TEST(codegen_cg48, operator_plus_value_result) {
    BEZEL_EXPECT_EQ(run(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator +(a: V, b: V): int => a.X + b.X; "
        "end V; "
        "class C public static method Main(): int "
        "  var a := new V(); a.X := 3; var b := new V(); b.X := 4; "
        "  return a + b; "
        "end Main; end C;"), 7);
}

BEZEL_TEST(codegen_cg48, operator_times) {
    BEZEL_EXPECT_EQ(run(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator *(a: V, b: V): int => a.X * b.X; "
        "end V; "
        "class C public static method Main(): int "
        "  var a := new V(); a.X := 6; var b := new V(); b.X := 7; "
        "  return a * b; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_cg48, operator_comparison) {
    BEZEL_EXPECT_EQ(run(
        // §25.2: comparison operators must be declared as a complete group.
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator <(a: V, b: V): bool => a.X < b.X; "
        "  public static method operator >(a: V, b: V): bool => a.X > b.X; "
        "  public static method operator <=(a: V, b: V): bool => a.X <= b.X; "
        "  public static method operator >=(a: V, b: V): bool => a.X >= b.X; "
        "end V; "
        "class C public static method Main(): int "
        "  var a := new V(); a.X := 3; var b := new V(); b.X := 5; "
        "  return (if a < b then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_cg48, operator_block_body_returns_object) {
    // v0.201 (#2048): a fresh-producing operator's class-typed result is
    // spelled `owned V` per the declaration-site static-return rule.
    BEZEL_EXPECT_EQ(run(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator +(a: V, b: V): owned V "
        "    var r := new V(); r.X := a.X + b.X; return r; "
        "  end; "
        "end V; "
        "class C public static method Main(): int "
        "  var a := new V(); a.X := 30; var b := new V(); b.X := 12; "
        "  var c := a + b; return c.X; "
        "end Main; end C;"), 42);
}

BEZEL_TEST(codegen_cg48, primitive_operators_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return 3 + 4 * 2 - 1; "
        "end Main; end C;"), 10);
}

// §25.1: unary operator overloads (`-`, `~`) on a user type.
BEZEL_TEST(codegen_cg48, unary_minus_overload) {
    BEZEL_EXPECT_EQ(run(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator -(a: V): int => 0 - a.X; "
        "end V; "
        "class C public static method Main(): int "
        "  var v := new V(); v.X := 5; return 0 - (-v); "
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg48, unary_complement_overload) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class V "
        "  public X: int := 0; "
        "  public static method operator ~(a: V): int => ~a.X; "
        "end V; "
        "class C public static method Main(): int "
        "  var v := new V(); v.X := 0; Console.WriteLine(~v); return 0; "
        "end Main; end C;"), "-1");
}

BEZEL_TEST(codegen_cg48, primitive_unary_unchanged) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "  var x := 5; Console.WriteLine(-x); return 0; "
        "end Main; end C;"), "-5");
}

// #1059 / §25 + §22: an operator overload on a GENERIC record/class is a
// (non-generic) static method whose param/return types use the declaring type's
// parameter `T`. It must specialize per-instantiation like a regular method;
// before the fix it was emitted once with the unspecialized `T`-placeholder
// layout and the specialized call args mismatched its parameter types (IR_FAIL
// "Call parameter type does not match function signature"). Non-generic
// operators and regular generic-record methods always worked; this is the
// generic × operator-overload combination.
BEZEL_TEST(codegen_cg48, generic_record_operator_int_1059) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Vec<T>(X: int) "
        "  public static method operator +(a: Vec<T>, b: Vec<T>): Vec<T> => "
        "    new Vec<T>(a.X + b.X); end Vec; "
        "class C public static method Main(): int "
        "  var c: Vec<int> := new Vec<int>(3) + new Vec<int>(4); return c.X; "
        "end Main; end C;"), 7);
}

// A non-`int` type argument (string) — the unspecialized i32 placeholder layout
// is genuinely wrong here, so this exercises real per-instantiation
// specialization (not a coincidental i32-matches-int case).
BEZEL_TEST(codegen_cg48, generic_record_operator_string_1059) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(V: T) "
        "  public static method operator +(a: Box<T>, b: Box<T>): Box<T> => "
        "    new Box<T>(a.V); end Box; "
        "class C public static method Main(): int "
        "  var a: Box<string> := new Box<string>(\"hi\"); "
        "  var b: Box<string> := new Box<string>(\"yo\"); "
        "  var c: Box<string> := a + b; return c.V.Length; "
        "end Main; end C;"), 2);
}

// Two distinct instantiations (P<int> and P<long>) in one program each get
// their own specialized operator.
BEZEL_TEST(codegen_cg48, generic_record_operator_multi_instantiation_1059) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P<T>(X: T) "
        "  public static method operator +(a: P<T>, b: P<T>): P<T> => "
        "    new P<T>(a.X); end P; "
        "class C public static method Main(): int "
        "  var i: P<int> := new P<int>(5) + new P<int>(6); "
        "  var l: P<long> := new P<long>(7L) + new P<long>(8L); "
        "  return i.X + int(l.X); "
        "end Main; end C;"), 12);
}

// A generic CLASS (reference type) operator returning `owned Vec<T>`.
BEZEL_TEST(codegen_cg48, generic_class_operator_1059) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Vec<T> public X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "  public static method operator +(a: Vec<T>, b: Vec<T>): owned Vec<T> => "
        "    new Vec<T>(a.X + b.X); end Vec; "
        "class C public static method Main(): int "
        "  using var c: Vec<int> := new Vec<int>(3) + new Vec<int>(4); return c.X; "
        "end Main; end C;"), 7);
}

// #1260 / §25 / §14.1: a user-defined operator on a GENERIC record, whose result
// is bound WITHOUT an explicit type annotation (`var r := a + b`), must be typed
// as the concrete instantiation `Box<int>`, not the unsubstituted `Box<T>`. The
// operator resolves and the value is correct, but the result type left `T`
// unsubstituted, so a later access `r.value` typed as the opaque param `T` — and
// a second operator on it (`r.value + 1`) wrongly reported "operator + not
// defined for `?`". (The existing 1059 tests above use an explicit annotation,
// which masks this — the inference path is what regressed.) Here `r.value` must
// resolve to `int` so `r.value + 1` type-checks and evaluates.
BEZEL_TEST(codegen_cg48, generic_record_operator_inferred_result_field_1260) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T) "
        "  public static method operator +(a: Box<T>, b: Box<T>): Box<T> => "
        "    new Box<T>(a.value); end Box; "
        "class C public static method Main(): int "
        "  var a := new Box<int>(10); var b := new Box<int>(20); "
        "  var r := a + b; return r.value + 1; "
        "end Main; end C;"), 11);
}

// #1260 sibling: the operator returns the bare param `T`; the inferred result
// `r` must be `int` (not opaque `T`) so `r + 1` type-checks.
BEZEL_TEST(codegen_cg48, generic_record_operator_inferred_bare_param_result_1260) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box<T>(value: T) "
        "  public static method operator +(a: Box<T>, b: Box<T>): T => a.value; "
        "end Box; "
        "class C public static method Main(): int "
        "  var a := new Box<int>(7); var b := new Box<int>(8); "
        "  var r := a + b; return r + 1; "
        "end Main; end C;"), 8);
}

// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix comment (kept for
// history): "#1219 / §25.2 / §14 / §9.5: `++` on a GENERIC record with a
// `new`-body operator ... After the fix: the per-instantiation substitution
// is established before emitMethodCall so the operator is specialized to
// match the concrete struct → clean IR, exit 6." §25.1/§17.6 retire the
// `operator ++`/`operator --` mechanism outright at v0.295 — the
// generic-specialization machinery #1219 fixed is now unreachable because
// the declaration itself is rejected first.
BEZEL_TEST(codegen_cg48, generic_record_increment_declaration_rejected_spec_25_1) {
    auto r = runAot(
        "module M; "
        "record Box<T>(V: int) "
        "  public static method operator ++(b: Box<T>): Box<T> => new Box<T>(b.V + 1); "
        "end Box; "
        "class C public static method Main(): int "
        "  var b: Box<string> := new Box<string>(5); b++; return b.V; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContainsOp(r, "operator `++` may not be overloaded (§25.1)"));
}

// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix comment (kept for
// history): "`++` on a generic record with a `with`-body operator ... After
// the fix: 10 + 3×1 = 13." Same retirement as above — the `with`-body
// operator's own declaration is rejected before any of the three `c++`
// calls are reached.
BEZEL_TEST(codegen_cg48, generic_record_increment_with_body_declaration_rejected_spec_25_1) {
    auto r = runAot(
        "module M; "
        "record Cnt<T>(V: int) "
        "  public static method operator ++(c: Cnt<T>): Cnt<T> => c with(V := c.V + 1); "
        "end Cnt; "
        "class C public static method Main(): int "
        "  var c: Cnt<string> := new Cnt<string>(10); c++; c++; c++; return c.V; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContainsOp(r, "operator `++` may not be overloaded (§25.1)"));
}

// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix comment (kept for
// history): "#1219 / §25.2: `--` on a generic record — same fix path as
// `++`." Same retirement, the `--` twin.
BEZEL_TEST(codegen_cg48, generic_record_decrement_declaration_rejected_spec_25_1) {
    auto r = runAot(
        "module M; "
        "record Box<T>(V: int) "
        "  public static method operator --(b: Box<T>): Box<T> => new Box<T>(b.V - 1); "
        "end Box; "
        "class C public static method Main(): int "
        "  var b: Box<string> := new Box<string>(20); b--; b--; return b.V; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContainsOp(r, "operator `--` may not be overloaded (§25.1)"));
}
