// fix #2128 / §28.10 (v0.180+, current v0.216) "using var initialized from a
// borrow": a TYPED `using var` whose initializer is one of the borrow shapes the
// spec lists there — beyond the bare-id (#2071), arm-meet (#1661), bind-atom
// (#2098), and bare-T-call (#1801) shapes already recognized — is a BORROW-PIN,
// NOT an adoption. Spec §28.10 "`using var` initialized from a borrow":
//
//   When the right-hand side of a `using var` declaration is a borrow rather
//   than an owned value (a method call without an `owned` return … an indexer
//   access, a field read, or … an *unmarked* read of a named owned binding),
//   the `using var` does **not** assume a disposal obligation … at scope end,
//   no `Dispose()` is called, because the value is not owned by this scope.
//
// Pre-fix, `handleLocalDecl` recognized a borrow only for the bare-id / arm-meet
// / bind-atom / bare-T-call shapes; a CONCRETE borrow-returning call (`o.Get()`
// where `Get(): Widget return _w;`), a FIELD read (`o._w`), and an INDEXER
// access (`arr[i]`) were NOT recognized, so `usingVarSourceIsBorrow` stayed
// clear. For a TYPED binding codegen's `classOfLocal` then returns the declared
// class unconditionally and `registerUsingDisposal` registers a scope-end
// dispose — double-freeing the borrow whose real owner also disposes it
// (confirmed heap-use-after-free under ASan: the typed using-var freed the
// Widget, then the owner's destructor freed the same object again).
//
// The fix recognizes those three shapes POSITIVELY (never off producerState's
// unreliable `Borrowed` verdict, which also parks fresh-owned producers —
// collection literals, `RawBytes.From`, iterator/subscription returns — that
// MUST keep their adoption). Field reads are distinguished from property/indexer
// GETTERS (which may return owned) by symbol resolution: only a FieldSymbol
// pins. A borrow-returning call is a directly-resolved user method whose every
// return is a bare receiver-owned-field borrow.
//
// This file pins each closed shape (ASan-clean exactly-once disposal, the source
// owner disposing, the pin disposing nothing) and asserts the OVER-REJECT
// controls keep adoption: an `owned`-returning call and an unbound `new`
// temporary are disposed exactly once by the using-var scope. Each test drives
// the AOT + generated-code-ASan/LSan oracle (runAotChecked) — without the fix
// the double-free fires ASan and `r.ran` is false; with the fix the run is clean
// and `Widget.live == 0` (net-zero live = disposed exactly once). // per §28.10
// #2128

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A class whose static `live` counter increments on construction and decrements
// on destruction: an exact net-0 dispose-once oracle (ctor +1 / dtor -1). A
// double-free additionally fires ASan under runAotChecked (r.ran == false).
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
// (1) §28.10 / #2128 PIN — a TYPED `using var` over a CONCRETE borrow-returning
// call (`o.Get()` where `Get(): Widget return _w;`, `_w` an `owned` field of the
// receiver). The call returns a borrow of the receiver's field; its real owner
// (`o`, whose scope-end disposal cascades to `_w`) disposes it exactly once. The
// typed using-var must NOT register a second dispose. This is the exact repro
// from the issue — a heap-use-after-free without the fix. // per §28.10 #2128
// ===========================================================================
BEZEL_TEST(using_var_typed_borrow_pin_2128,
           typed_using_var_over_borrow_returning_call_pins_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Owner "
        "  private owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "  public method Get(): Widget return _w; end Get; "  // borrow of _w
        "end Owner; "
        "class P "
        "  public static method Run(): int "
        "    using var o := new Owner(); "
        "    using var y: Widget := o.Get(); "  // TYPED pin — must NOT dispose
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "               // 0 — disposed exactly once (by o)
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // fails without the fix: ASan double-free
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // Widget.live == 0 ⇒ exactly-once disposal
}

// ===========================================================================
// (2) §28.10 / #2128 PIN — a TYPED `using var` over a FIELD read: both a
// cross-object read (`o._w`, a public owned field of another object) and a bare
// implicit-`this` read (`_w` inside the owner). A field read is a lend —
// ownership stays with the field's holder, which disposes it. // per §28.10 #2128
// ===========================================================================
BEZEL_TEST(using_var_typed_borrow_pin_2128,
           typed_using_var_over_field_read_pins_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Owner "
        "  public owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "  public method PinInside(): int "
        "    using var y: Widget := _w; "        // bare implicit-this field read
        "    return y.Ping(); "
        "  end PinInside; "
        "end Owner; "
        "class P "
        "  public static method Run(): int "
        "    using var o := new Owner(); "
        "    using var y: Widget := o._w; "      // cross-object field read
        "    var a := o.PinInside(); "
        "    return y.Ping() + a; "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    if Widget.live <> 0 then return 1; end; "  // 0 — disposed once (by o)
        "    if r <> 2 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // fails without the fix: ASan double-free
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (3) §28.10 / §28.11 / #2128 PIN — a TYPED `using var` over an INDEXER access
// (`arr[0]` over an `OwningArray<Widget>`). Indexer-get yields a borrow (source:
// the collection, a transit point — §28.11 element-slot table); the collection
// owns the element and disposes it on its own scope-end cascade. The typed
// using-var must not double-free it. // per §28.10, §28.11 #2128
// ===========================================================================
BEZEL_TEST(using_var_typed_borrow_pin_2128,
           typed_using_var_over_indexer_get_pins_spec_28_11) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class P "
        "  public static method Run(): int "
        "    using var arr := new OwningArray<Widget>(); "
        "    arr.Add(new Widget()); "
        "    using var y: Widget := arr[0]; "    // indexer-get -> borrow (pin)
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "               // 0 — disposed once (by arr)
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // fails without the fix: ASan double-free
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (4) §28.10 / #2128 CONTROL (over-reject guard) — a TYPED `using var` over an
// `owned`-returning call (`f.Make(): owned Widget => new Widget()`) still ADOPTS.
// `f.Make()` is a fresh owned production the using-var owns and disposes exactly
// once; the fix's POSITIVE recognition must NOT pin it (a pin would LEAK the
// fresh Widget). Guards against the over-reject the issue calls out for
// owned-returning getters (tcs.Task.Result-style). // per §28.10 #2128
// ===========================================================================
BEZEL_TEST(using_var_typed_borrow_pin_2128,
           typed_using_var_over_owned_returning_call_still_adopts_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Factory "
        "  public method Make(): owned Widget return new Widget(); end Make; "
        "end Factory; "
        "class P "
        "  public static method Run(): int "
        "    using var f := new Factory(); "
        "    using var y: Widget := f.Make(); "  // owned return -> ADOPT (dispose)
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "               // 0 — adopted & disposed once by y
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // Widget.live == 0 ⇒ no leak, disposed once
}

// ===========================================================================
// (5) §28.10 / #2128 CONTROL (over-reject guard) — a TYPED `using var` over an
// unbound `new` temporary still ADOPTS and disposes exactly once. The plainest
// owned-production shape must keep its disposal after the fix. // per §28.10 #2128
// ===========================================================================
BEZEL_TEST(using_var_typed_borrow_pin_2128,
           typed_using_var_over_new_temporary_still_adopts_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class P "
        "  public static method Run(): int "
        "    using var y: Widget := new Widget(); "  // fresh owned -> ADOPT
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "               // 0 — adopted & disposed once
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
