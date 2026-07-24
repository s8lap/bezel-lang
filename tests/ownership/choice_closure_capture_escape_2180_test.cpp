// Spec-derived ACCEPT controls for issue #2180 — a borrow-bearing CHOICE
// carrier (a choice local, or a case-arm payload binding of borrow-bearing
// type) captured by an ESCAPING closure is rejected (§9.7 / §35.4 / §35.5,
// v0.220: "Borrow-bearing choices are subject to every rule of this section,
// with 'record' read as 'record or choice' throughout" — including the
// closure-capture escape bullet; §35.5: "a pattern binding is a borrow whose
// source is the scrutinee's source").
//
// The REJECT side — a choice local captured by a returned lambda (route 1), a
// choice local captured by a closure stored to an outer local (route 1, #861),
// and a case-arm payload binding captured by a returned lambda (route 2) — is
// pinned by the lit tests
// (tests/lit/ownership/choice_closure_capture_escape_2180__*). This file pins
// the ACCEPT side: §9.7's rule constrains only a closure that ESCAPES the
// source's scope, and only a BORROW-BEARING carrier, so the nearby legal shapes
// must stay accepted and ASan-clean — the over-reject failure mode.
//
// These accepts capture the choice LOCAL (not a case-arm binding): a returned
// closure capturing a case-arm binding trips a PRE-EXISTING, orthogonal codegen
// LEAK of the capture environment (present on main, unrelated to the ownership
// analysis this fix touches — tracked separately), so the legal *arm-binding*
// captures are pinned as compile-only lit accepts
// (choice_closure_capture_escape_2180__arm_binding_*_accepted.bzl) instead, which
// verify non-over-rejection without exercising the leaky runtime path.
//
// per §9.7 (v0.220), §35.4, §35.5, §28.3

#include <string>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// ===========================================================================

// §35.4: a borrow-bearing choice LOCAL captured by a closure INVOKED LOCALLY
// (bound to `g`, called while `local` is still alive) — the non-escaping
// sibling of route 1. The closure never escapes the source's scope, so the
// §9.7 closure bullet does not apply and the widened carrier predicate must not
// turn this into a rejection. == 7.
BEZEL_TEST(choice_closure_capture_escape_2180, choice_local_capture_local_invoke_accepted) {
    std::string src =
        "module M; record Holder(S: ArraySpan<int>); "
        "choice Box Got(h: Holder), Nothing; end Box; "
        "class C "
        "  public static method Sink(v: Box): int "
        "    var acc := 0; "
        "    case v of Got(var y) => do acc := y.S[0]; end, Nothing => do end end; "
        "    return acc; "
        "  end Sink; "
        "  public static method Main(): int "
        "    var local := new Array<int>(); local.Add(7); "
        "    var hh := new Holder(local.AsSpan()); "
        "    var b := Box.Got(hh); "
        "    var g := lambda(): int => C.Sink(b); "
        "    return g(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §35.4: an ESCAPING (returned) closure capturing a borrow-bearing choice LOCAL
// whose carried source is a PARAMETER — the choice views the caller's array,
// which outlives the callee frame (a parameter is not an escape-bound
// obligation, mirroring the #2144 param-sourced accept). The choice-local
// provenance resolves b -> hh -> a (a param), so carriedEscapeBoundSourceLiveAt
// finds no escape-bound source and it stays accepted. == 7.
BEZEL_TEST(choice_closure_capture_escape_2180, param_source_choice_local_returned_accepted) {
    std::string src =
        "module M; record Holder(S: ArraySpan<int>); "
        "choice Box Got(h: Holder), Nothing; end Box; "
        "class C "
        "  public static method Sink(v: Box): int "
        "    var acc := 0; "
        "    case v of Got(var y) => do acc := y.S[0]; end, Nothing => do end end; "
        "    return acc; "
        "  end Sink; "
        "  public static method Make(a: Array<int>): () -> int "
        "    var hh := new Holder(a.AsSpan()); "
        "    var b := Box.Got(hh); "
        "    return lambda(): int => C.Sink(b); "
        "  end Make; "
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(7); "
        "    var f := C.Make(arr); "
        "    return f(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §35.4.3: a VALUE-LIKE choice (no borrow-carrying payload) LOCAL captured by a
// RETURNED closure — a value-like choice is an ordinary copyable value with no
// lifetime constraint, so the escape is legal. The carrier predicate must not
// widen to "any choice" (isBorrowBearingChoice is false for a value-like one).
// == 42.
BEZEL_TEST(choice_closure_capture_escape_2180, value_like_choice_local_returned_accepted) {
    std::string src =
        "module M; choice VBox Some(n: int), None; end VBox; "
        "class C "
        "  public static method SinkV(v: VBox): int "
        "    var acc := 0; "
        "    case v of Some(var y) => do acc := y; end, None => do end end; "
        "    return acc; "
        "  end SinkV; "
        "  public static method Make(): () -> int "
        "    var b := VBox.Some(42); "
        "    return lambda(): int => C.SinkV(b); "
        "  end Make; "
        "  public static method Main(): int "
        "    var f := C.Make(); "
        "    return f(); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
