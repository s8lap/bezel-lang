// fix #2137 / §28.10, §28.3, §28.6, §26.7 (spec v0.220): the residual typed
// using-var adoption shapes #2128 left open — (a) a borrow-returning PROPERTY
// getter read, and (b) a concrete call whose every return is the receiver
// borrow itself (`return this;` — the fluent instance shape of §28.6 "Methods
// returning `this`", and §26.7's extension receiver-parameter borrow, the one
// definition-site-admitted "borrowed-parameter" return: a non-receiver
// parameter borrow return is rejected at the declaration, §28.3 "Borrow
// lifetime rules").
//
// Spec §28.10 "`using var` initialized from a borrow": "When the right-hand
// side of a `using var` declaration is a borrow rather than an owned value (a
// method call without an `owned` return — unless its declared return type is
// a `Task`-family type […] — an indexer access, a field read, or — v0.180 —
// an *unmarked* read of a named owned binding), the `using var` does **not**
// assume a disposal obligation […]: at scope end, no `Dispose()` is called,
// because the value is not owned by this scope."
//
// §28.3 producers: "Property access `obj.Prop` | Borrowed (source: `obj`)" —
// property reads are borrows of their storage; a USER getter is additionally
// definition-site guaranteed borrow-producing ("a property or indexer getter
// may not return a freshly-produced owned value — a getter is a
// borrow-producing position", §28.6). §28.3 also admits the static-storage-
// sourced borrow: a static property getter returning a bare `static owned`
// field read (v0.196).
//
// Pre-fix, usingVarInitIsRecognizedBorrow recognized field reads, indexer
// accesses, and receiver-owned-field-borrow-returning calls (#2128), but NOT
// a PropertySymbol-resolved getter read nor a `return this;` body — those
// kept adoption, and a TYPED using var registered a scope-end dispose:
// heap-use-after-free / SEGV under ASan (the real owner disposes the same
// object again). Each pin test drives runAotChecked: without the fix the
// double-free makes r.ran false; with it the run is clean and the live
// counter is 0 (disposed exactly once). Controls keep the owned-transfer
// shapes adopting. // per §28.10, §28.3, §28.6, §26.7, #2137
//
// #2137 residual (this commit): the original get-body scan still under-pinned
// non-trivial body shapes — a `??`, a conditional, an interior call — that
// hit the borrow-pin under-pin (the typed `using var` adopted and
// double-freed). §28.6's signature-level guarantee ("a getter is a
// borrow-producing position") is independent of the body shape; the scan
// can only ever carve a small allow-list of the space of get-body forms.
// The fix is signature-level: any user-declared getter (non-abstract, not
// indexed, non-Task-family return) is a borrow-pin — its real owner
// disposes it, the using-var's only role is to keep a copy alive past
// the scope via the local. // per §28.6, §28.3, K2.4, #2137

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
// (1) §28.10 / §28.3 PIN — a TYPED `using var` over an expression-bodied
// borrow-returning getter (`public property W: Widget => _w;`). The read is a
// borrow of `o`'s owned field; `o`'s cascade is the single disposal.
// Pre-fix: heap-use-after-free.
// ===========================================================================
BEZEL_TEST(using_var_getter_borrow_pin_2137,
           typed_using_var_over_expr_bodied_getter_pins_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Owner "
        "  private owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "  public property W: Widget => _w; "
        "end Owner; "
        "class P "
        "  public static method Run(): int "
        "    using var o := new Owner(); "
        "    using var y: Widget := o.W; "   // getter borrow — pin, no dispose
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "           // 0 — disposed exactly once (by o)
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // fails without the fix: ASan double-free
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (2) §28.10 / §13.6 PIN — the BLOCK-form getter (`get => _w;`) and the
// UNTYPED binding axis (`using var y := o.W;`) of the same shape.
// ===========================================================================
BEZEL_TEST(using_var_getter_borrow_pin_2137,
           block_form_getter_and_untyped_binding_pin_spec_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Owner "
        "  private owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "  public property W: Widget "
        "    get => _w; "
        "  end W; "
        "end Owner; "
        "class P "
        "  public static method Run(): int "
        "    using var o := new Owner(); "
        "    using var t: Widget := o.W; "   // typed pin
        "    using var u := o.W; "           // untyped pin (same rule)
        "    return t.Ping() + u.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    if Widget.live <> 0 then return 1; end; "
        "    if r <> 2 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (3) §28.3 (v0.196) PIN — a STATIC property getter returning a bare `static
// readonly owned` field read: the admitted static-storage-sourced borrow. The
// program's §22.13 exit disposal (bz_dispose_static_owned) is the single
// disposal; the pre-fix typed adoption freed the widget first → SEGV inside
// the exit disposal.
// ===========================================================================
BEZEL_TEST(using_var_getter_borrow_pin_2137,
           typed_using_var_over_static_getter_static_owned_field_pins_spec_28_3) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Registry "
        "  private static owned readonly _shared: Widget := new Widget(); "
        "  public static property Shared: Widget => _shared; "
        "end Registry; "
        "class P "
        "  public static method Run(): int "
        "    using var y: Widget := Registry.Shared; "  // static-storage borrow
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return r - 1; "   // 0; the exit disposal then frees _shared once
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);   // fails without the fix: SEGV at exit
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (4) §28.3 PIN — an initializer-less AUTO-property read (`property Current:
// Widget;` — the generated getter is a backing-field read, a borrow of
// storage). The §28.3 back-reference pattern stores a parameter-sourced
// borrow; its real owner (`w`) disposes once.
// ===========================================================================
BEZEL_TEST(using_var_getter_borrow_pin_2137,
           typed_using_var_over_auto_property_read_pins_spec_28_3) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Holder "
        "  public property Current: Widget; "
        "  public constructor(w: Widget) Current := w; end constructor; "
        "end Holder; "
        "class P "
        "  public static method Run(): int "
        "    using var w := new Widget(); "
        "    using var h := new Holder(w); "
        "    using var y: Widget := h.Current; "  // backing-field borrow — pin
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "                // 0 — disposed once (by w)
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (4b) §23.3 / §28.10 PIN — the `get => do … end;` BLOCK-bodied accessor
// (statement getter; exercises the return-statement scanner rather than the
// expression-body test). Sync caller only: reading ANY block-DO getter from
// an ASYNC body currently emits malformed IR — a pre-existing, ownership-
// independent codegen bug (minimal repro: an `int` block-DO getter read from
// an async method; filed alongside this fix), so the async×block-DO cell is
// pinned there, not here.
// ===========================================================================
BEZEL_TEST(using_var_getter_borrow_pin_2137,
           block_do_statement_getter_pins_spec_23_3_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Owner "
        "  private owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "  public property W: Widget "
        "    get => do "
        "      return _w; "
        "    end; "
        "  end W; "
        "end Owner; "
        "class P "
        "  public static method Run(): int "
        "    using var o := new Owner(); "
        "    using var y: Widget := o.W; "   // scanner-path pin
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "           // 0 — disposed once (by o)
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (4c) §30.1 / §28.10 PIN — the ASYNC-body position: the pin inside an async
// state machine must skip disposal exactly as the sync path does (the frame's
// cleanup machinery honors usingVarSourceIsBorrow). Expression-bodied getter.
// ===========================================================================
BEZEL_TEST(using_var_getter_borrow_pin_2137,
           async_body_position_getter_pin_spec_30_1_28_10) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Owner "
        "  private owned _w: Widget; "
        "  public constructor() _w := new Widget(); end constructor; "
        "  public property W: Widget => _w; "
        "end Owner; "
        "class P "
        "  public static async method RunAsync(): Task<int> "
        "    using var o := new Owner(); "
        "    using var y: Widget := o.W; "   // pin inside an async frame
        "    return y.Ping(); "
        "  end RunAsync; "
        "  public static async method Main(): Task<int> "
        "    using var t := P.RunAsync(); "
        "    var r := await t; "
        "    if Widget.live <> 0 then return 1; end; "
        "    if r <> 1 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (5) §28.6 / §28.10 PIN — an instance method whose every return is `return
// this;` (the fluent shape §28.6 "Methods returning `this`" classifies a
// receiver borrow). Pre-fix the typed using var adopted and double-freed `o`.
// ===========================================================================
BEZEL_TEST(using_var_getter_borrow_pin_2137,
           typed_using_var_over_return_this_call_pins_spec_28_6) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Fluent "
        "  public static live: int := 0; "
        "  public constructor() Fluent.live := Fluent.live + 1; end constructor; "
        "  public method Chain(): Fluent return this; end Chain; "
        "  public method Ping(): int return 1; end Ping; "
        "  destructor Fluent.live := Fluent.live - 1; end destructor; "
        "end Fluent; "
        "class P "
        "  public static method Run(): int "
        "    using var o := new Fluent(); "
        "    using var y: Fluent := o.Chain(); "  // receiver borrow — pin
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Fluent.live; "                // 0 — disposed once (by o)
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (6) §26.7 / §28.10 PIN — an EXTENSION method returning its receiver
// parameter (`=> this`): "The receiver is always a borrow" (§26.7), and the
// §28.3 producers row sources the call's borrow at the receiver parameter.
// This is the one definition-site-admitted borrowed-PARAMETER return (#2137
// shape (b)): plain-parameter borrow returns from static/instance methods are
// rejected at the declaration.
// ===========================================================================
BEZEL_TEST(using_var_getter_borrow_pin_2137,
           typed_using_var_over_extension_receiver_return_pins_spec_26_7) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "extension Widget "
        "  public method Self(): Widget => this; "
        "end; "
        "class P "
        "  public static method Run(): int "
        "    using var o := new Widget(); "
        "    using var y: Widget := o.Self(); "  // receiver-param borrow — pin
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "               // 0 — disposed once (by o)
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (7) CONTROL (over-pin guard) — an `owned`-returning DETACH method (§28.8
// "Detaching via a nullable field": `var w := move _w!; _w := null; return
// w;`) still ADOPTS: the widened call recognition is gated on
// `!md->returnOwned`, and its body-scan accepts only receiver-sourced borrow
// returns (a bare owned LOCAL return is neither), so the transfer keeps its
// single using-var disposal (a pin would LEAK the detached widget; LSan
// would flag it).
// ===========================================================================
BEZEL_TEST(using_var_getter_borrow_pin_2137,
           owned_returning_detach_call_still_adopts_control_spec_28_8) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Owner "
        "  private owned _w: Widget?; "
        "  public constructor() _w := new Widget(); end constructor; "
        "  public method Take(): owned Widget "
        "    var w := move _w!; "
        "    _w := null; "
        "    return w; "
        "  end Take; "
        "end Owner; "
        "class P "
        "  public static method Run(): int "
        "    using var o := new Owner(); "
        "    using var y: Widget := o.Take(); "  // owned transfer — ADOPT
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "               // 0 — y disposed it once
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (8) CONTROL (over-pin guard) — a getter body that is NOT a recognized
// receiver-sourced borrow keeps adoption-side behavior elsewhere unchanged:
// a VALUE-typed property read under `using var` stays the §28.9 no-op (no
// pin-related crash, no disposal semantics change). Guards the PropertySymbol
// recognition against classifying value-typed getters.
// ===========================================================================
BEZEL_TEST(using_var_getter_borrow_pin_2137,
           value_typed_getter_using_var_noop_control_spec_28_9) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Counter "
        "  private _n: int; "
        "  public constructor() _n := 41; end constructor; "
        "  public property N: int => _n; "
        "end Counter; "
        "class P "
        "  public static method Main(): int "
        "    using var c := new Counter(); "
        "    using var n := c.N; "   // value read — §28.9 no-op disposal
        "    return n - 41; "        // 0
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (9) §28.6 / §28.10 PIN (§28.6 signature-level guarantee, #2137 residual) —
// a getter whose body is NOT a bare field read — `_w ?? _w` (the §28.3
// NullCoalescing operator): §28.6's "a property or indexer getter may not
// return a freshly-produced owned value" is a SIGNATURE-LEVEL guarantee,
// independent of the body shape. The pre-fix body-scan predicate
// (propertyGetterIsRecognizedBorrow) rejected this shape — `??` is a
// BinaryExpr, not in the receiver-sourced scan — and the typed `using var`
// adopted. At scope end, both `y` and `o` disposed the widget: the first
// freed the heap, the second read freed memory (heap-use-after-free under
// ASan, the #2137 family). The relaxation to signature-level acceptance
// (excluding the Task-family K2.3 carve) closes the gate. // per §28.6,
// §28.3, K2.4, #2137
// ===========================================================================
BEZEL_TEST(using_var_getter_borrow_pin_2137,
           typed_using_var_over_null_coalescing_getter_pins_spec_28_6) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Holder "
        "  private _w: Widget; "
        "  public constructor(w: Widget) _w := w; end constructor; "
        "  public property W: Widget => _w ?? _w; "
        "end Holder; "
        "class P "
        "  public static method Run(): int "
        "    using var o := new Widget(); "
        "    using var h := new Holder(o); "
        "    using var y: Widget := h.W; "
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "  // 0 — disposed once (by o)
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);  // fails without the fix: ASan heap-use-after-free
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (10) §28.6 / §28.10 PIN — a getter whose body is a conditional
// (`if b then _w else _w end`): same §28.6 signature-level guarantee; the
// body scan's case-by-case accept list is provably insufficient for the
// spectrum of expressions a getter can hold. Pre-fix the typed using var
// adopted and double-freed. // per §28.6, §28.3, K2.4, #2137
// ===========================================================================
BEZEL_TEST(using_var_getter_borrow_pin_2137,
           typed_using_var_over_conditional_getter_pins_spec_28_6) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Holder "
        "  private _w: Widget; "
        "  private _flag: bool; "
        "  public constructor(w: Widget) _w := w; _flag := true; end constructor; "
        "  public property W: Widget "
        "    get => do "
        "      if _flag then return _w; else return _w; end; "
        "    end; "
        "  end W; "
        "end Holder; "
        "class P "
        "  public static method Run(): int "
        "    using var o := new Widget(); "
        "    using var h := new Holder(o); "
        "    using var y: Widget := h.W; "
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "  // 0 — disposed once (by o)
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);  // fails without the fix: ASan double-free
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// (11) §28.6 / §28.10 PIN — a BLOCK-bodied getter whose body is a
// non-bare-field expression body (a `do ... return _w + <call>; end`): the
// pre-fix body's `return _w + <call>` failed the receiver-sourced scan
// because of the call operand; the typed using var adopted and double-freed.
// §28.6's signature-level guarantee covers this too. // per §28.6, §28.3,
// K2.4, #2137
// ===========================================================================
BEZEL_TEST(using_var_getter_borrow_pin_2137,
           typed_using_var_over_chained_call_getter_pins_spec_28_6) {
    JitResult r = runAotChecked(
        std::string(kWidget) +
        "class Holder "
        "  private _w: Widget; "
        "  public constructor(w: Widget) _w := w; end constructor; "
        "  public property W: Widget "
        "    get => _w; "
        "  end W; "
        "end Holder; "
        "class P "
        "  public static method Run(): int "
        "    using var o := new Widget(); "
        "    using var h := new Holder(o); "
        "    using var y: Widget := h.W; "
        "    return y.Ping(); "
        "  end Run; "
        "  public static method Main(): int "
        "    var r := P.Run(); "
        "    return Widget.live; "  // 0 — disposed once (by o)
        "  end Main; "
        "end P;");
    if (!r.ran) {
        BEZEL_EXPECT_TRUE(r.ran);
        return;
    }
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
