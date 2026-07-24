// fix #2166 / §28.10 (v0.220): the `using var` borrow-pin classification
// (`handleLocalDecl`'s `usingVarSourceIsBorrow` flag — see the #2128/#2137
// derivation) is set by the dataflow's `visitStmt` walk for every `LocalDecl`
// reached IN-LINE in a method body. A `LocalDecl` inside a LAMBDA body is
// reached only by the lambda's own dataflow, which the ownership pass does NOT
// run (lambdas carry their body as an `Expr`; `checkCapturedOwnedConsumeInStmt`
// and `checkLambdaTaskFamilyBorrowReturns` are the only lambda walks, and
// neither classifies the local-decl layer). Codegen
// (`registerUsingDisposal`, `classOfLocal`) then treats the typed `using var`
// as an adoption and registers a scope-end dispose — double-freeing the
// borrow whose real owner also disposes it (heap-use-after-free under ASan
// for the field-read shape; the same shape family as #2128/#2137, just in
// lambda position).
//
// Spec §28.10 "`using var` initialized from a borrow" (v0.220): "When the
// right-hand side of a `using var` declaration is a borrow rather than an
// owned value (a method call without an `owned` return — unless its declared
// return type is a `Task`-family type — an indexer access, a field read, or
// — v0.180 — an *unmarked* read of a named owned binding), the `using var`
// does **not** assume a disposal obligation […]: at scope end, no `Dispose()`
// is called, because the value is not owned by this scope." The rule must
// fire for a `using var` inside a lambda body exactly as it does for one in
// a method body — the lambda body is just a nested scope, not a different
// language.
//
// The pre-fix repro: heap-use-after-free inside `bz$Owner$DisposeCore`
// (read of freed memory), freed by the lambda's typed `using var y` (the
// adapter's wrong adoption), with the real owner `o` disposing the same
// object at scope end. ASan oracle: `r.ran == false` without the fix, `r.ran`
// → clean and `Widget.live == 0` (disposed exactly once by `o`) with the fix.
//
// The fix runs the existing `handleLocalDecl` predicate layer
// (`usingVarInitIsRecognizedBorrow`) over every LocalDecl inside every
// lambda body in the method — same predicate the dataflow relies on for
// in-method `using var`, applied via a structural pre-pass that walks the
// AST. Each lambda body is its own scope (fresh state map), exactly as a
// method body is. The fix preserves the method-body and async behavior
// (the dataflow path is unchanged) and does not pin true owned/adopted
// values (the predicate is positive, not off producerState's Borrowed
// verdict). // per §28.10 v0.220, #2166

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// ctor +1 / dtor -1 net-zero dispose-once oracle (double-free → ASan).
const char* kWidget =
    "module M; "
    "class Widget "
    "  public static live: int := 0; "
    "  public constructor() Widget.live := Widget.live + 1; end constructor; "
    "  public method Ping(): int return 1; end Ping; "
    "  destructor Widget.live := Widget.live - 1; end destructor; "
    "end Widget; ";

}  // namespace

// ===========================================================================
// (1) §28.10 / #2166 PIN — the issue's exact repro: a TYPED `using var` over
// a cross-object field read (`o._w`, an `owned` Widget field of another
// object) inside a LAMBDA body. A field read is a lend — the field's holder
// owns the value and disposes it; the typed lambda-body using-var must NOT
// register a second dispose. Pre-fix: heap-use-after-free.
// ===========================================================================
BEZEL_TEST(using_var_borrow_pin_lambda_body_2166,
           typed_using_var_over_field_read_in_lambda_body_pins_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Owner "
        "  public owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "end Owner; "
        "class P "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      using var o := new Owner(); "
        "      using var y: Widget := o._w; "  // §28.10 field-read — MUST pin
        "      return y.Ping(); "
        "    end; "
        "    var r := f(); "
        "    return Widget.live; "
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // fails without the fix: ASan double-free
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // Widget.live == 0 ⇒ exactly-once disposal
}

// ===========================================================================
// (2) §28.10 / #2166 PIN — the SIBLING GETTER shape: a TYPED `using var`
// over a borrow-returning property getter read inside a lambda body. The
// getter is definition-site guaranteed borrow-producing (§28.6 "a property
// or indexer getter may not return a freshly-produced owned value"); its
// lambda-body using-var must pin, not adopt. This is the #2137 sibling shape
// in the residual lambda position. Pre-fix: heap-use-after-free.
// ===========================================================================
BEZEL_TEST(using_var_borrow_pin_lambda_body_2166,
           typed_using_var_over_getter_in_lambda_body_pins_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Owner "
        "  private owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "  public property W: Widget => _w; "
        "end Owner; "
        "class P "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      using var o := new Owner(); "
        "      using var y: Widget := o.W; "   // getter borrow — pin
        "      return y.Ping(); "
        "    end; "
        "    var r := f(); "
        "    return Widget.live; "
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // fails without the fix: ASan double-free
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (3) §28.10 / #2166 PIN — the BORROW-RETURNING CALL sibling: a TYPED
// `using var` over a concrete borrow-returning method call (`o.Get()` where
// `Get(): Widget return _w;`) inside a lambda body. The receiver-owned-field
// borrow-return is the #2128 shape, in the residual lambda position. Pre-fix:
// heap-use-after-free.
// ===========================================================================
BEZEL_TEST(using_var_borrow_pin_lambda_body_2166,
           typed_using_var_over_borrow_returning_call_in_lambda_body_pins_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Owner "
        "  private owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "  public method Get(): Widget return _w; end Get; "
        "end Owner; "
        "class P "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      using var o := new Owner(); "
        "      using var y: Widget := o.Get(); "  // borrow-returning call
        "      return y.Ping(); "
        "    end; "
        "    var r := f(); "
        "    return Widget.live; "
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // fails without the fix: ASan double-free
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (4) §28.10 / #2166 PIN — the INDEXER sibling: a TYPED `using var` over
// an indexer access (`arr[0]` over an `OwningArray<Widget>`) inside a
// lambda body. Indexer-get is a borrow out (§28.11 element-slot table);
// the using-var must pin, not adopt. Pre-fix: heap-use-after-free.
// ===========================================================================
BEZEL_TEST(using_var_borrow_pin_lambda_body_2166,
           typed_using_var_over_indexer_in_lambda_body_pins_spec_28_11) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class P "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      using var arr := new OwningArray<Widget>(); "
        "      arr.Add(new Widget()); "
        "      using var y: Widget := arr[0]; "  // indexer-get — borrow-pin
        "      return y.Ping(); "
        "    end; "
        "    var r := f(); "
        "    return Widget.live; "
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // fails without the fix: ASan double-free
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (5) §28.10 / #2166 CONTROL — a TYPED `using var` over an `owned`-returning
// call inside a lambda body ADOPTS (keeps adoption). The `factory.Make()`
// returns a fresh owned Widget; the using-var owns and disposes it. The
// fix's POSITIVE recognition must NOT pin it (a pin would LEAK the fresh
// Widget). Guards against the over-reject: the predicate passes for
// borrow-returning calls (a receiver-owned-field-borrow body) and FAILS for
// owned-returning calls (a fresh-`new` body). // per §28.10, #2166
// ===========================================================================
BEZEL_TEST(using_var_borrow_pin_lambda_body_2166,
           typed_using_var_over_owned_returning_call_in_lambda_body_adopts_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Factory "
        "  public method Make(): owned Widget return new Widget(); end Make; "
        "end Factory; "
        "class P "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      using var fc := new Factory(); "
        "      using var y: Widget := fc.Make(); "  // owned return -> ADOPT
        "      return y.Ping(); "
        "    end; "
        "    var r := f(); "
        "    return Widget.live; "                  // 0 — disposed exactly once
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (6) §28.10 / #2166 CONTROL — a TYPED `using var` over an unbound `new`
// temporary inside a lambda body ADOPTS. The plainest owned-production
// shape must keep its disposal after the fix. Guards against an over-pin
// regression. // per §28.10, #2166
// ===========================================================================
BEZEL_TEST(using_var_borrow_pin_lambda_body_2166,
           typed_using_var_over_new_temporary_in_lambda_body_adopts_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class P "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      using var y: Widget := new Widget(); "  // fresh owned -> ADOPT
        "      return y.Ping(); "
        "    end; "
        "    var r := f(); "
        "    return Widget.live; "
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (7) §28.10 / #2166 CONTROL — an UNTYPED `using var` over a cross-object
// field read inside a lambda body pins. The bare-identifier pinned-binding
// branch (`states_[src].state in {Owned, Borrowed}`) is the pre-#2128 path
// that the fix reuses; this guards that the same path still fires for
// lambda-body LocalDecls. // per §28.10, #2166
// ===========================================================================
BEZEL_TEST(using_var_borrow_pin_lambda_body_2166,
           untyped_using_var_over_field_read_in_lambda_body_pins_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Owner "
        "  public owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "end Owner; "
        "class P "
        "  public static method Main(): int "
        "    var f := lambda(): int => do "
        "      using var o := new Owner(); "
        "      using var y := o._w; "  // UNTYPED — same pred path
        "      return y.Ping(); "
        "    end; "
        "    var r := f(); "
        "    return Widget.live; "
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (8) §28.10 / #2166 METHOD-BODY CONTROL — the SAME typed `using var` over
// a field read in an ORDINARY METHOD body is clean (the #2128 dataflow
// path handles it). This test guards that the pre-pass does NOT regress
// the method-body path — the dataflow walk still runs unchanged and
// `handleLocalDecl` is the same predicate either way. // per §28.10, #2128
// ===========================================================================
BEZEL_TEST(using_var_borrow_pin_lambda_body_2166,
           method_body_typed_using_var_over_field_read_stays_pin_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Owner "
        "  public owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "end Owner; "
        "class P "
        "  public static method Run(): int "
        "    using var o := new Owner(); "
        "    using var y: Widget := o._w; "
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (9) §28.10 / #2166 ASYNC-METHOD CONTROL — the SAME typed `using var`
// over a field read inside an ASYNC method body is clean (the #2128
// dataflow path handles async bodies too; the pre-pass is a no-op for
// async locals because the pre-pass walks the AST structurally and
// `handleLocalDecl`'s async exemption is per-decl). The async method
// returns `Task<int>` (required by §30.1). // per §28.10, #2128
// ===========================================================================
BEZEL_TEST(using_var_borrow_pin_lambda_body_2166,
           async_method_typed_using_var_over_field_read_stays_pin_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Owner "
        "  public owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "end Owner; "
        "class P "
        "  public static async method Run(): Task<int> "
        "    using var o := new Owner(); "
        "    using var y: Widget := o._w; "
        "    return y.Ping(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var t := P.Run(); "
        "    var r := await t; "
        "    return Widget.live; "
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
