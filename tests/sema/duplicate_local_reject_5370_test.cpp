// Spec-derived regression test for issue #5370 (spec §21.1, v0.314).
//
// THE DEFECT. Six sites in ExprTypecheck_Exprs.cpp register an inline `out var`
// declaration (plain `out var`, `out owned var`, `out using var` at both method
// and constructor call sites) but none of them checked whether the name was
// already in scope — duplicate inline declarations were silently accepted,
// with the second shadowing the first silently.
//
// THE FIX. Each of the six sites now checks `currentScope_->names` before
// registering and emits: "a local named `X` is already declared in this scope
// (§21.1, v0.314)".  The setter's implicit `value` param is exempt
// (Appendix B / §23.3) — tested separately in setter_implicit_value_exempt_5401.
//
// ONE ROW PER SITE (method / constructor × out var / out owned var / out using var).
// Control rows prove distinct names still compile.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"        // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// -----------------------------------------------------------------------------
// Site 1: plain `out var` at method call site — inline local declaration
// §21.1 (#5370, v0.314)
// -----------------------------------------------------------------------------
BEZEL_TEST(duplicate_local_reject_5370, out_var_method_call_duplicate) {
    auto r = runAot(
        "module M;"
        "class C"
        "  public method TryGet(ok: bool, out result: int) end TryGet;"
        "end C;"
        "class P"
        "  public static method Main(): int"
        "    var c := new C();"
        "    var result := 0;"
        "    c.TryGet(true, out var result);"
        "    return result;"
        "  end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "a local named `result` is already declared in this scope (§21.1"));
}

// -----------------------------------------------------------------------------
// Site 2: `out owned var` at method call site — inline owned local declaration
// §21.1 (#5370, v0.314)
// -----------------------------------------------------------------------------
BEZEL_TEST(duplicate_local_reject_5370, out_owned_var_method_call_duplicate) {
    auto r = runAot(
        "module M;"
        "class C"
        "  public method TryOwn(ok: bool, out owned result: C) end TryOwn;"
        "end C;"
        "class D end D;"
        "class P"
        "  public static method Main(): int"
        "    var result: D := new D();"
        "    var c := new C();"
        "    c.TryOwn(true, out var result);"
        "    return 0;"
        "  end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "a local named `result` is already declared in this scope (§21.1"));
}

// -----------------------------------------------------------------------------
// Site 3: `out using var` at method call site — inline using-scoped handle
// §21.1 (#5370, v0.314)
//
// `out using var` is the call-site form for builtin extraction methods
// (OwningSet.TryPop, Channel.TryReceive, etc.) whose parameter is declared
// `out owned` in the stdlib.  The duplicate check is in the same
// `a.modifier == "out using var"` arm as Site 1 (plain `out var`), so
// we test with a distinct builtin (OwningSet.TryPop vs TryGet).
// -----------------------------------------------------------------------------
BEZEL_TEST(duplicate_local_reject_5370, out_using_var_method_call_duplicate) {
    auto r = runAot(
        "module M;"
        "class R "
        "  public Val: int; "
        "  public constructor(v: int) Val := v; end constructor; "
        "  destructor end destructor; "
        "  public override method GetHashCode(): int => Val; "
        "  public override method Equals(other: Object?): bool "
        "    if other is R then var o := other as R; return o!.Val = Val; end; "
        "    return false; end Equals; "
        "end R; "
        "class P"
        "  public static method Main(): int"
        "    using var s := new OwningSet<R>();"
        "    s.Add(new R(1));"
        "    var b: R? := null;"
        "    s.TryPop(out using var b);"
        "    return 0;"
        "  end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "a local named `b` is already declared in this scope (§21.1"));
}

// -----------------------------------------------------------------------------
// Site 4: plain `out var` at constructor call site — inline local declaration
// §21.1 (#5370, v0.314)
// -----------------------------------------------------------------------------
BEZEL_TEST(duplicate_local_reject_5370, out_var_constructor_call_duplicate) {
    auto r = runAot(
        "module M;"
        "class C"
        "  public constructor(ok: bool, out result: int) end constructor;"
        "end C;"
        "class P"
        "  public static method Main(): int"
        "    var result := 0;"
        "    var c := new C(true, out var result);"
        "    return result;"
        "  end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "a local named `result` is already declared in this scope (§21.1"));
}

// -----------------------------------------------------------------------------
// Site 5: `out owned var` at constructor call site — inline owned local
// §21.1 (#5370, v0.314)
//
// Constructor declared with `out owned n: Item`, called as `new Widget("x",
// out owned var n)` where `n` is the inline declaration.  Duplicate with a
// pre-existing local `n` triggers §21.1.
// -----------------------------------------------------------------------------
BEZEL_TEST(duplicate_local_reject_5370, out_owned_var_constructor_call_duplicate) {
    auto r = runAot(
        "module M;"
        "class Item end Item;"
        "class Widget "
        "  public constructor(s: string, out owned n: Item) n := new Item(); end constructor; "
        "end Widget;"
        "class P"
        "  public static method Main(): int "
        "    var n: Item := new Item();"
        "    var w := new Widget(\"x\", out var n);"
        "    return 0; "
        "  end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "a local named `n` is already declared in this scope (§21.1"));
}

// -----------------------------------------------------------------------------
// Site 6: `out using var` at constructor call site — inline using-scoped handle
// §21.1 (#5370, v0.314)
//
// Same constructor as Site 5 but called with `out using var n` (the using form
// of the same `out owned` parameter).  Duplicate with pre-existing `n`.
// -----------------------------------------------------------------------------
BEZEL_TEST(duplicate_local_reject_5370, out_using_var_constructor_call_duplicate) {
    auto r = runAot(
        "module M;"
        "class Item end Item;"
        "class Widget "
        "  public constructor(s: string, out owned n: Item) n := new Item(); end constructor; "
        "end Widget;"
        "class P"
        "  public static method Main(): int "
        "    var n: Item := new Item();"
        "    var w := new Widget(\"x\", out using var n);"
        "    return 0; "
        "  end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "a local named `n` is already declared in this scope (§21.1"));
}

// -----------------------------------------------------------------------------
// Control: distinct inline out var names still compile cleanly (method call)
//
// Uses `int.TryParse` (a builtin that actually assigns its out param) to avoid
// the "not definitely assigned" error from a stub method declaration.
// -----------------------------------------------------------------------------
BEZEL_TEST(duplicate_local_reject_5370, control_distinct_names_method_call) {
    auto r = runAot(
        "module M;"
        "class P"
        "  public static method Main(): int"
        "    var s := \"123\";"
        "    var x := 0;"
        "    if int.TryParse(s, out var result) then"
        "      return result;"
        "    end;"
        "    return -1;"
        "  end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 123);
}

// -----------------------------------------------------------------------------
// Control: distinct inline out var names still compile cleanly (constructor)
//
// Uses `int.TryParse` builtin for method call; for constructor control we use
// a simple stub constructor whose out param IS definitely assigned because we
// write it in the constructor body.
// -----------------------------------------------------------------------------
BEZEL_TEST(duplicate_local_reject_5370, control_distinct_names_constructor_call) {
    auto r = runAot(
        "module M;"
        "class C"
        "  private _v: int;"
        "  public constructor(ok: bool, out result: int)"
        "    if ok then _v := 42; result := 42;"
        "    else _v := 0; result := 0;"
        "    end;"
        "  end constructor;"
        "end C;"
        "class P"
        "  public static method Main(): int"
        "    var x := 0;"
        "    var c := new C(true, out var result);"
        "    return result;"
        "  end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// -----------------------------------------------------------------------------
// Control: a pre-existing non-inline local shadowed by an inline out var is
// also caught (the scope already has `result` when `out var result` is parsed)
// -----------------------------------------------------------------------------
BEZEL_TEST(duplicate_local_reject_5370, control_inline_shadows_prior_local) {
    auto r = runAot(
        "module M;"
        "class C"
        "  public method TryGet(ok: bool, out result: int) end TryGet;"
        "end C;"
        "class P"
        "  public static method Main(): int"
        "    var result := 42;"
        "    var c := new C();"
        "    c.TryGet(true, out var result);"
        "    return result;"
        "  end Main;"
        "end P;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "a local named `result` is already declared in this scope (§21.1"));
}
