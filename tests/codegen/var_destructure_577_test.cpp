// Regression tests for issue #577: `var`-prefixed bindings in a record
// destructuring pattern arm were not visible in the arm body.
//
// §19.3: `Pt(var x, var y)` is equivalent to `Pt(x, y)` — both introduce
// bindings of the destructured field values into the arm scope.
//
// Before the fix, sema skipped `BindingPattern` sub-patterns in a
// DestructuringPattern's sub-pattern list (it only handled nakedBinding),
// so the bound names were never inserted into the arm scope. Codegen
// similarly lacked a slot for them. Now both are handled identically to
// the bare naked-binding form.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ── §19.3 example 1: `Pt(var a, var b)` — both var-bindings visible ──────────

// §19.3: `Pt(var a, var b)` extracts both fields; arm body computes a + b.
// Expected: 10 + 20 = 30.
BEZEL_TEST(codegen_577_var_destructure, var_bindings_both_visible_spec_19_3) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(10, 20); "
        "  return (case p of "
        "    Pt(var a, var b) => a + b, "
        "    _ => 0); "
        "end Main; end C;"), 30);
}

// §19.3: `Pt(var a, var b)` — use only first bound name (a).
// Expected: 10.
BEZEL_TEST(codegen_577_var_destructure, var_binding_first_field_spec_19_3) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(10, 20); "
        "  return (case p of "
        "    Pt(var a, var b) => a, "
        "    _ => 0); "
        "end Main; end C;"), 10);
}

// §19.3: `Pt(var a, var b)` — use only second bound name (b).
// Expected: 20.
BEZEL_TEST(codegen_577_var_destructure, var_binding_second_field_spec_19_3) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(10, 20); "
        "  return (case p of "
        "    Pt(var a, var b) => b, "
        "    _ => 0); "
        "end Main; end C;"), 20);
}

// ── §19.3 example 2: mixed `Pt(var a, b)` — one var-binding, one naked ───────

// §19.3: Mixed `Pt(var a, b)` — first sub-pattern uses var-prefix, second
// uses the bare naked-binding form. Both must be visible in the arm body.
// Expected: 3 + 4 = 7.
BEZEL_TEST(codegen_577_var_destructure, mixed_var_and_naked_binding_spec_19_3) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Pair(A: int, B: int); "
        "class C public static method Main(): int "
        "  var p := new Pair(3, 4); "
        "  return (case p of "
        "    Pair(var a, b) => a + b, "
        "    _ => 0); "
        "end Main; end C;"), 7);
}

// §19.3: Mixed `Pair(a, var b)` — second sub-pattern uses var-prefix.
// Expected: 5 (value of b).
BEZEL_TEST(codegen_577_var_destructure, naked_then_var_binding_spec_19_3) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Pair(A: int, B: int); "
        "class C public static method Main(): int "
        "  var p := new Pair(3, 5); "
        "  return (case p of "
        "    Pair(a, var b) => b, "
        "    _ => 0); "
        "end Main; end C;"), 5);
}

// ── §19.3 regression: bare `Pt(a, b)` form still works after the fix ─────────

// §19.3: Verify that the original bare `Pt(a, b)` form was not broken by
// the fix that added var-binding support.
// Expected: 3 + 4 = 7.
BEZEL_TEST(codegen_577_var_destructure, bare_form_regression_spec_19_3) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(3, 4); "
        "  return (case p of "
        "    Point(x, y) => x + y, "
        "    _ => 0); "
        "end Main; end C;"), 7);
}

// ── §19.3: var-bindings in statement-position case ────────────────────────────

// §19.3 applies to both expression-position and statement-position case.
// In statement-position `case`, the arm body is a `do … end` block.
// Expected: a + b = 10 + 20 = 30 (returned via a local variable).
BEZEL_TEST(codegen_577_var_destructure, var_binding_case_stmt_spec_19_3) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C public static method Main(): int "
        "  var p := new Pt(10, 20); "
        "  var r := 0; "
        "  case p of "
        "    Pt(var a, var b) => do r := a + b; end "
        "  end; "
        "  return r; "
        "end Main; end C;"), 30);
}

// ── §19.3: mixed constant + var-binding sub-patterns ─────────────────────────

// §19.3 / §19.4: `Pair(0, var b)` — first sub-pattern is a constant (must
// match 0), second is a var-binding. Arm fires only when first field == 0.
// Expected: b = 7 when Pair(0, 7).
BEZEL_TEST(codegen_577_var_destructure, const_then_var_binding_spec_19_3) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Pair(A: int, B: int); "
        "class C public static method Main(): int "
        "  var p := new Pair(0, 7); "
        "  return (case p of "
        "    Pair(0, var b) => b, "
        "    _ => -1); "
        "end Main; end C;"), 7);
}

// Constant + var-binding: arm must NOT fire when the constant sub-pattern
// does not match. Expected: -1 (fallthrough to _ arm).
// The value is printed (not returned) so the assertion sees all of it: the
// exit status is 8-bit, and -1 would read as 255.
BEZEL_TEST(codegen_577_var_destructure, const_then_var_binding_no_match_spec_19_3) {
    auto r = runAot(
        "module M; "
        "record Pair(A: int, B: int); "
        "class C public static method Main(): int "
        "  var p := new Pair(5, 7); "
        "  var n := (case p of "
        "    Pair(0, var b) => b, "
        "    _ => -1); "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n");
}

// #726 / §19.3: NESTED record destructuring in a case arm binds the inner name.
// `Outer(Inner(v), b)` was rejected with "unresolved identifier `v`" — sema did
// not recurse into a nested DestructuringPattern sub-pattern (codegen already
// bound the value). Both case-expr and case-stmt handlers now recurse.
BEZEL_TEST(codegen_577_var_destructure, nested_destructuring_case_expr_726) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Inner(V: int); "
        "record Outer(A: Inner, B: int); "
        "class C public static method Main(): int "
        "  var o := new Outer(new Inner(7), 3); "
        "  var r := (case o of "
        "    Outer(Inner(v), b) => v + b, "
        "    _ => 0); "
        "  return r; "  // 7 + 3 = 10
        "end Main; end C;"), 10);
}

// #726: nested destructuring in a case STATEMENT arm.
BEZEL_TEST(codegen_577_var_destructure, nested_destructuring_case_stmt_726) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Inner(V: int); "
        "record Outer(A: Inner, B: int); "
        "class C public static method Main(): int "
        "  var o := new Outer(new Inner(8), 4); "
        "  var r := 0; "
        "  case o of "
        "    Outer(Inner(v), b) => do r := v * b; end, "
        "    _ => do r := 99; end, "
        "  end; "
        "  return r; "  // 8 * 4 = 32
        "end Main; end C;"), 32);
}

// #1271: calling a METHOD on a NAKED destructure binding (`Box(a) => a.M()`)
// must not crash the compiler. The naked binding's LocalSymbol previously had
// its `decl` set to a `reinterpret_cast<Node*>(&PositionalPattern)` — a non-Node
// pointer — so any `dynamic_cast<...>(loc->decl)` in codegen (e.g. the
// Subscription-disposal check on a `.Dispose()` call) read a bogus vtable and
// SIGSEGV'd. The naked binding now anchors on a real BindingPattern Node. Here a
// plain method call on the binding compiles and runs (returns the field value).
BEZEL_TEST(codegen_577_var_destructure, method_call_on_naked_binding_no_crash_1271) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "record Box(V: int); "
        "class C public static method Main(): int "
        "  var b := new Box(41); "
        "  return (case b of "
        "    Box(a) => a.ToString().Length + a, "  // "41".Length(2) + 41 = 43
        "    _ => 0,); "
        "end Main; end C;"), 43);
}

// #1281 (§19.3): a NESTED destructure that binds a CLASS/reference inner field
// and reads a member through it (`Outer(Inner(x)) => x.Id`). The nested binding's
// sema declType was left null (the nested walk only registered the name), so
// `x.Id` could not resolve — in a case EXPRESSION the arm body's `lowerExpr`
// returned null and `case.end` was left unterminated (IR verification failure);
// in a case STATEMENT it miscompiled (arm mis-selection → wrong value). The nested
// binding now takes its inner record field's type. Expression position:
BEZEL_TEST(codegen_577_var_destructure, nested_class_binding_member_access_expr_1281) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Widget public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; end Widget; "
        "record Inner(W: Widget); record Outer(I: Inner); "
        "class C public static method Main(): int "
        "  using var w := new Widget(9); "
        "  var inner := new Inner(w); var o := new Outer(inner); "
        "  return (case o of Outer(Inner(x)) => x.Id, _ => -1,); "  // 9
        "end Main; end C;"), 9);
}

// #1281 statement position (was a miscompile to -1): the same nested class binding
// in a case STATEMENT arm reads the right value.
BEZEL_TEST(codegen_577_var_destructure, nested_class_binding_member_access_stmt_1281) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; "
        "class Widget public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; end Widget; "
        "record Inner(W: Widget); record Outer(I: Inner); "
        "class C public static method Main(): int "
        "  using var w := new Widget(9); "
        "  var inner := new Inner(w); var o := new Outer(inner); "
        "  case o of "
        "    Outer(Inner(x)) => do return x.Id; end, "  // 9
        "    _ => do return -1; end, "
        "  end; "
        "  return -2; "
        "end Main; end C;"), 9);
}
