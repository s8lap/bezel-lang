// fix #880 #881 / §27.6 / §24.4: a bare unqualified instance-method name used
// in value position (event handler `Fired += Handle`, `return Handle`) must
// bind the implicit `this` receiver — exactly as the qualified `this.Handle`
// form does in a MemberAccessExpr. Without the fix, the env block of the
// produced closure held a null/garbage receiver, causing a silent no-op for
// event handlers (#880, returns 0) and a SIGSEGV at the call site (#881).
//
// §27.6 ("greeter.Greet" / "bound instance method") + §24.4 (named handler
// `Fired += Handle` canonical form inside the declaring class) together
// establish that a bare method name resolves to a bound delegate capturing
// `this`, not a naked function pointer.
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #881: `return Handle;` (bare) produces a bound delegate; `h(5)` must
// mutate the correct object's field and not SIGSEGV.
BEZEL_TEST(bare_method_value_880_881, bare_return_binds_this_881) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public V: int := 0; "
        "  public method Add(n: int) V := V + n; end Add; "
        "end Box; "
        "class C "
        "  private _box: Box; "
        "  public constructor(b: Box) this._box := b; end constructor; "
        "  public method GetBare(): (int) -> () return Handle; end GetBare; "
        "  public method Handle(n: int) _box.V := _box.V + n; end Handle; "
        "  public static method Main(): int "
        "    using var box := new Box(); "
        "    using var c := new C(box); "
        "    var h := c.GetBare(); "
        "    h(5); "
        "    return box.V; "  // expected 5; was SIGSEGV
        "  end Main; "
        "end C;"), 5);
}

// #880: `Fired += Handle` (bare) must register a handler whose receiver is
// `this`; after Fire(3) the handler must have mutated _box.V.
BEZEL_TEST(bare_method_value_880_881, bare_event_subscribe_fires_handler_880) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public V: int := 0; end Box; "
        "class Source "
        "  public event Fired: int; "
        "  private _box: Box; "
        "  public constructor(b: Box) this._box := b; end constructor; "
        "  public method HookBare() Fired += Handle; end HookBare; "
        "  public method Handle(v: int) _box.V := _box.V + v; end Handle; "
        "  public method Fire(v: int) Fired.Raise(v); end Fire; "
        "end Source; "
        "class C "
        "  public static method Main(): int "
        "    using var box := new Box(); "
        "    using var s := new Source(box); "
        "    s.HookBare(); "
        "    s.Fire(3); "
        "    return box.V; "  // expected 3; was 0
        "  end Main; "
        "end C;"), 3);
}

// Fired.Subscribe(Handle) — bare in argument position.
// §24.4 / §27.6: Subscribe accepts any function value; bare Handle is a bound
// delegate carrying `this`, so the handler fires and mutates _box.V.
BEZEL_TEST(bare_method_value_880_881, bare_subscribe_arg_fires_handler) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box3 public V: int := 0; end Box3; "
        "class Source3 "
        "  public event Fired: int; "
        "  private _box: Box3; "
        "  public constructor(b: Box3) this._box := b; end constructor; "
        "  public method HookSubscribe() "
        "    using var _ := Fired.Subscribe(Handle); "
        "    Fired.Raise(7); "
        "  end HookSubscribe; "
        "  public method Handle(v: int) _box.V := _box.V + v; end Handle; "
        "end Source3; "
        "class C "
        "  public static method Main(): int "
        "    using var box := new Box3(); "
        "    using var s := new Source3(box); "
        "    s.HookSubscribe(); "
        "    return box.V; "  // expected 7
        "  end Main; "
        "end C;"), 7);
}

// Regression: qualified `return this.Handle` must still produce the correct
// bound delegate (was always working; must not regress).
BEZEL_TEST(bare_method_value_880_881, qualified_this_handle_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public V: int := 0; end Box; "
        "class C "
        "  private _box: Box; "
        "  public constructor(b: Box) this._box := b; end constructor; "
        "  public method GetQualified(): (int) -> () return this.Handle; end GetQualified; "
        "  public method Handle(n: int) _box.V := _box.V + n; end Handle; "
        "  public static method Main(): int "
        "    using var box := new Box(); "
        "    using var c := new C(box); "
        "    var h := c.GetQualified(); "
        "    h(5); "
        "    return box.V; "  // expected 5
        "  end Main; "
        "end C;"), 5);
}

// Regression: `obj.Method` on a DIFFERENT object (not this) must still bind
// THAT object's receiver. §27.6: instance is captured.
BEZEL_TEST(bare_method_value_880_881, other_object_method_ref_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Counter "
        "  public V: int := 0; "
        "  public method Inc(n: int) V := V + n; end Inc; "
        "end Counter; "
        "class C "
        "  public static method Main(): int "
        "    using var c1 := new Counter(); "
        "    var f := c1.Inc; "
        "    f(10); "
        "    f(5); "
        "    return c1.V; "  // expected 15
        "  end Main; "
        "end C;"), 15);
}

// Regression: a bare method value called multiple times keeps the receiver
// alive and valid for the object's lifetime.
BEZEL_TEST(bare_method_value_880_881, bare_method_value_multiple_calls) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Acc "
        "  public V: int := 0; "
        "  public method Add(n: int) V := V + n; end Add; "
        "  public method GetAdd(): (int) -> () return Add; end GetAdd; "
        "  public static method Main(): int "
        "    using var a := new Acc(); "
        "    var f := a.GetAdd(); "
        "    f(3); f(7); f(10); "
        "    return a.V; "  // expected 20
        "  end Main; "
        "end Acc;"), 20);
}

// Regression: a bare static-method reference must NOT capture `this`
// (no receiver for a static method). §27.6: static methods have no instance.
// This path was always correct (goes through the `isStaticMethod` branch);
// verify it is unaffected.
BEZEL_TEST(bare_method_value_880_881, static_method_ref_no_spurious_capture) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class MathHelper "
        "  public static method Double(n: int): int return n * 2; end Double; "
        "  public static method Main(): int "
        "    var f := MathHelper.Double; "  // qualified static ref
        "    return f(21); "  // expected 42
        "  end Main; "
        "end MathHelper;"), 42);
}
