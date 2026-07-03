// Ownership tests for §28.14's CALL-BOUNDED DEFAULT (v0.177, #1419).
//
// "A function-typed parameter declared without `scoped` is call-bounded: the
// callee receives the callback for the duration of the call only. Within the
// callee the callback may be invoked, bound to locals, and passed on as an
// argument to another call-bounded parameter; it may not be stored in a field
// of any object, stored into any collection that is itself reachable beyond
// the call (a parameter, a borrow, or a field — a purely local collection is
// a local binding), returned, captured by a closure that itself escapes the
// callee (judged by these same rules, applied transitively), or forwarded
// into a `scoped` parameter position. … These obligations are
// compiler-verified for user-authored callees."  — §28.14, v0.177
//
// "The caller side is correspondingly unrestricted: any closure —
// capture-bearing or not — may be passed to a call-bounded parameter, since
// every captured source trivially outlives the call."  — §28.14, v0.177

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContains(const bezel::test::OwnershipResult& r,
                  const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ===========================================================================
// Allowed uses (§28.14 v0.177): invoke, bind to locals, plain→plain forward
// ===========================================================================

// §28.14 v0.177: an invoke-only callee is clean, and the CALLER may pass a
// capture-bearing closure to its plain parameter (any closure is admissible —
// every captured source outlives the call).
BEZEL_TEST(call_bounded_2814, invoke_only_callee_and_capturing_caller_clean) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public method Val(): int return 3; end Val; "
        "  destructor end; "
        "end Res; "
        "class C "
        "  public static method Apply(f: () -> int): int "
        "    return f(); "
        "  end Apply; "
        "  public static method Main(): int "
        "    using var res := new Res(); "
        "    return C.Apply(() => res.Val()); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 v0.177: binding the callback to a local and invoking through it is a
// local binding — clean.
BEZEL_TEST(call_bounded_2814, bind_to_local_and_invoke_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Twice(f: () -> int): int "
        "    var g := f; "
        "    return g() + g(); "
        "  end Twice; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 v0.177: passing the callback on to another PLAIN function-typed
// parameter is allowed (plain→plain forwarding).
BEZEL_TEST(call_bounded_2814, plain_to_plain_forwarding_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Inner(g: () -> int): int return g(); end Inner; "
        "  public static method Outer(f: () -> int): int "
        "    return C.Inner(f); "
        "  end Outer; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 v0.177: storing into a PURELY LOCAL collection is a local binding —
// clean.
BEZEL_TEST(call_bounded_2814, store_into_local_collection_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Run(f: () -> int): int "
        "    var xs: Array<() -> int> := []; "
        "    xs.Add(f); "
        "    return xs[0](); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 v0.177: a closure capturing the callback that is itself only invoked
// locally does not escape — clean (the transitive judgment admits it).
BEZEL_TEST(call_bounded_2814, capturing_closure_invoked_locally_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Run(f: () -> int): int "
        "    var h := () => f(); "
        "    return h(); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 v0.177 no-regression control: the stdlib lazy query operators keep
// their stdlib-certified storing signatures — forwarding a plain predicate
// parameter into `Where` stays clean.
BEZEL_TEST(call_bounded_2814, forward_to_lazy_query_operator_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method CountMatching(xs: Array<int>, "
        "                                     pred: (int) -> bool): int "
        "    return xs.Where(pred).Count(); "
        "  end CountMatching; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 v0.177 no-regression CONTROL (negative): the lazy query operators'
// storing behavior stays on the certified legacy path — returning a query
// object whose predicate captures an owned scope-bound local still escapes
// the closure with the returned value, and is still rejected.
BEZEL_TEST(call_bounded_2814, return_lazy_query_capturing_owned_still_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res "
        "  public method Limit(): int return 2; end Limit; "
        "  destructor end; "
        "end Res; "
        "class C "
        "  public static method Bad(xs: Array<int>): IEnumerable<int> "
        "    using var res := new Res(); "
        "    return xs.Where((x) => x > res.Limit()); "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Forbidden sink: field store
// ===========================================================================

// §28.14 v0.177: the callback may not be stored in a field of any object —
// bare-field form. The diagnostic names the fix.
BEZEL_TEST(call_bounded_2814, field_store_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private _f: (() -> int)?; "
        "  public method Set(f: () -> int) "
        "    _f := f; "
        "  end Set; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(
        r, "parameter `f` is call-bounded; declare it `scoped` to store it"));
}

// §28.14 v0.177: this-qualified and other-object field stores are the same
// sink.
BEZEL_TEST(call_bounded_2814, other_object_field_store_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  public _fn: (() -> int)?; "
        "  destructor end; "
        "end Holder; "
        "class C "
        "  public static method Stash(f: () -> int, h: Holder) "
        "    h._fn := f; "
        "  end Stash; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
}

// §28.14 v0.177 transitivity: a LOCAL bound from the parameter is the same
// value — storing the alias in a field is the same sink.
BEZEL_TEST(call_bounded_2814, field_store_via_local_alias_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private _f: (() -> int)?; "
        "  public method Set(f: () -> int) "
        "    var g := f; "
        "    _f := g; "
        "  end Set; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
}

// ===========================================================================
// Forbidden sink: collection store (beyond the call)
// ===========================================================================

// §28.14 v0.177: storing into a PARAMETER-reachable collection is rejected.
BEZEL_TEST(call_bounded_2814, store_into_param_collection_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Enlist(f: () -> int, xs: Array<() -> int>) "
        "    xs.Add(f); "
        "  end Enlist; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "collection reachable beyond the call"));
}

// §28.14 v0.177: storing into a FIELD-reachable collection is rejected.
BEZEL_TEST(call_bounded_2814, store_into_field_collection_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private _handlers: Array<() -> int> := []; "
        "  public method Register(f: () -> int) "
        "    _handlers.Add(f); "
        "  end Register; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "collection reachable beyond the call"));
}

// §28.14 v0.177: the indexer-store form (`xs[0] := f`) through a parameter
// collection is the same sink.
BEZEL_TEST(call_bounded_2814, indexer_store_into_param_collection_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Plant(f: () -> int, xs: Array<() -> int>) "
        "    xs[0] := f; "
        "  end Plant; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
}

// §28.14 v0.177 transitivity: a closure capturing the callback stored into a
// beyond-call collection escapes the callee — rejected.
BEZEL_TEST(call_bounded_2814, capturing_closure_into_field_collection_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private _handlers: Array<() -> int> := []; "
        "  public method Register(f: () -> int) "
        "    _handlers.Add(() => f()); "
        "  end Register; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
}

// ===========================================================================
// Forbidden sink: return (incl. escaping-closure capture, transitively)
// ===========================================================================

// §28.14 v0.177: the callback may not be returned.
BEZEL_TEST(call_bounded_2814, return_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Pass(f: () -> int): () -> int "
        "    return f; "
        "  end Pass; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
}

// §28.14 v0.177: a closure capturing the callback that ESCAPES by return is
// rejected (the spec's recorded #1427 example: composition over call-bounded
// parameters is inexpressible).
BEZEL_TEST(call_bounded_2814, escaping_capturing_closure_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Compose(f: (int) -> int, g: (int) -> int)"
        ": (int) -> int "
        "    return (x: int) => g(f(x)); "
        "  end Compose; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
}

// §28.14 v0.177 transitivity: the escape judgment follows local bindings —
// a bound capturing closure returned later is the same escape.
BEZEL_TEST(call_bounded_2814, escaping_closure_via_local_alias_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Sneak(f: () -> int): () -> int "
        "    var h := () => f(); "
        "    return h; "
        "  end Sneak; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
}

// §28.14 v0.177: writing the callback through an out/ref parameter hands it
// beyond the call — the return sink in another syntax.
BEZEL_TEST(call_bounded_2814, out_param_write_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Leak(f: () -> int, out g: () -> int) "
        "    g := f; "
        "  end Leak; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
}

// ===========================================================================
// Forbidden sink: forwarding into a `scoped` parameter position
// ===========================================================================

// §28.14 v0.177: the callee cannot see its argument's capture set, so it
// cannot vouch for the §28.14 admission — forward-to-scoped is rejected, and
// the diagnostic names the fix (declare THIS parameter scoped).
BEZEL_TEST(call_bounded_2814, forward_to_scoped_param_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private scoped _cb: (() -> ())?; "
        "  public method Install(scoped cb: () -> ()) _cb := cb; end Install; "
        "  public method Relay(f: () -> ()) "
        "    Install(f); "
        "  end Relay; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
    BEZEL_EXPECT_TRUE(diagContains(r, "scoped"));
}

// §28.14 v0.177 transitivity: a closure CAPTURING the call-bounded parameter
// bound to a scoped position is the same violation.
BEZEL_TEST(call_bounded_2814, capturing_closure_to_scoped_param_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private scoped _cb: (() -> ())?; "
        "  public method Install(scoped cb: () -> ()) _cb := cb; end Install; "
        "  public method Relay(f: () -> ()) "
        "    Install(() => f()); "
        "  end Relay; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
}

// CONTROL: forwarding a `scoped` parameter to a scoped position on `this`
// stays admitted (§28.14 v0.175 forwarding rule — unchanged by v0.177).
BEZEL_TEST(call_bounded_2814, scoped_to_scoped_forward_still_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private scoped _cb: (() -> ())?; "
        "  public method Install(scoped cb: () -> ()) _cb := cb; end Install; "
        "  public method Relay(scoped f: () -> ()) "
        "    Install(f); "
        "  end Relay; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
