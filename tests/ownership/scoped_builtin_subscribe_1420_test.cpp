// Ownership tests for the #1420 builtin `scoped` flip — §28.14 (v0.175
// admission, v0.177 revision) applied to the builtin subscribe-and-store
// callees whose stdlib signatures declare the handler `scoped`:
//
//   EventSource<T>.Subscribe / AsyncEventSource<T>.Subscribe /
//   IEvent<T>.Subscribe / IAsyncEvent<T>.Subscribe   (stdlib §6.5, spec §24.3)
//   CancellationToken.Register                        (stdlib §6.1)
//
// All return `owned Subscription` (stdlib §2.11), so the §28.14 bounding
// object resolves per form 2: the call must initialize a `using var`/`await
// using var` local for a CAPTURE-BEARING handler to be admitted; capture-free
// handlers are always admitted. Disposing the Subscription severs the
// registration — quiescence by removal (§28.14, §24.6).
//
// As of v0.180 (#1541) `EventSource<T>.Add` / `AsyncEventSource<T>.Add` (the
// `event += handler` desugaring) ARE `scoped` too (§24.3): the stored handler
// admits under §28.14 with the bounding object resolved to the event's
// containing instance (form 4). §24.5's own capture-bearing `+=` example
// captures `this`, which form 4 admits. `Remove` (`-=`) stays call-bounded.
// The full forms-3/4 admission is exercised in event_add_scoped_1541_test.

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

bool hasErrorContaining(const bezel::test::OwnershipResult& r,
                        const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// Res (a disposable captured source), Button (a sync event), Beacon (an async
// event); `body` is spliced in as Prog's Main body.
std::string prog(const std::string& body) {
    return std::string(
        "module M; import System.Threading; "
        "class Res "
        "  public method Ping() end Ping; "
        "  destructor end; "
        "end Res; "
        "class Button "
        "  public event Clicked: int; "
        "  public method Fire() Clicked.Raise(1); end Fire; "
        "end Button; "
        "class Beacon "
        "  public async event Pulsed: int; "
        "end Beacon; "
        "class Prog "
        "  public static method Main(): int ") + body +
        "  end Main; "
        "end Prog;";
}

}  // namespace

// ===========================================================================
// EventSource<T>.Subscribe — sync events (§24.3 / stdlib §6.5)
// ===========================================================================

// §28.14 form 2 (owned return initializing a `using var`): a capture-bearing
// handler whose captured source is declared BEFORE the bounding local is
// ADMITTED.
BEZEL_TEST(scoped_builtin_1420, subscribe_using_var_capture_admitted) {
    auto r = parseAndCheckOwnership(prog(
        "    using var res := new Res(); "
        "    var b := new Button(); "
        "    using var sub := b.Clicked.Subscribe(lambda(n) => res.Ping()); "
        "    b.Fire(); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14: a capture-bearing handler whose subscription lands in a PLAIN `var`
// local (not `using var`) has no bounding object — rejected.
BEZEL_TEST(scoped_builtin_1420, subscribe_plain_var_capture_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    using var res := new Res(); "
        "    var b := new Button(); "
        "    var sub := b.Clicked.Subscribe(lambda(n) => res.Ping()); "
        "    sub.Dispose(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "capture-free"));
}

// §28.14: "capture-free callbacks are always admitted" — the same plain-`var`
// shape with a capture-free handler is clean.
BEZEL_TEST(scoped_builtin_1420, subscribe_plain_var_capture_free_admitted) {
    auto r = parseAndCheckOwnership(prog(
        "    var b := new Button(); "
        "    var sub := b.Clicked.Subscribe(lambda(n) => ()); "
        "    b.Fire(); "
        "    sub.Dispose(); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14: a capture of a VALUE-TYPED local is not a lifetime-constrained
// source (its capture box outlives every scope) — admitted without a bound.
BEZEL_TEST(scoped_builtin_1420, subscribe_value_typed_capture_admitted) {
    auto r = parseAndCheckOwnership(prog(
        "    var total := 0; "
        "    var b := new Button(); "
        "    var sub := b.Clicked.Subscribe(lambda(n) => do total := total + n; end); "
        "    b.Fire(); "
        "    sub.Dispose(); "
        "    return total; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 declaration-order: the captured source is declared AFTER the
// bounding `using var`, so it dies before the subscription — rejected.
BEZEL_TEST(scoped_builtin_1420, subscribe_declaration_order_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    var b := new Button(); "
        "    using var sub := b.Clicked.Subscribe(lambda(_) => ()); "
        "    return 0; "));
    // Control: capture-free is fine even with nothing declared before.
    // (fix #4968: the handler takes the `int` payload — stdlib §6.5's
    // `(T) -> ()`; the former zero-parameter `lambda()` was ill-typed and is
    // now rejected at the Subscribe argument, v0.307.)
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto r2 = parseAndCheckOwnership(prog(
        "    var b := new Button(); "
        "    using var sub := b.Clicked.Subscribe(lambda(n) => res.Ping()); "
        "    using var res := new Res(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r2.diagnostics.hasErrors());
}

// §28.14: a capture-bearing handler on a DISCARDED subscription (bare call
// statement) has no bounding object — rejected by the admission (in addition
// to the §2.11/#949 dropped-Subscription error).
BEZEL_TEST(scoped_builtin_1420, subscribe_discarded_capture_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    using var res := new Res(); "
        "    var b := new Button(); "
        "    b.Clicked.Subscribe(lambda(n) => res.Ping()); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "capture-free"));
}

// §28.14 freeze: the captured source may not be disposed while the
// subscription's scope is live.
BEZEL_TEST(scoped_builtin_1420, subscribe_freeze_dispose_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    var res := new Res(); "
        "    var b := new Button(); "
        "    using var sub := b.Clicked.Subscribe(lambda(n) => res.Ping()); "
        "    res.Dispose(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "frozen"));
}

// §24.3 / §27.6: a bare method-reference handler is admitted (a static method
// captures nothing; an instance method captures `this`, which outlives every
// local of this frame).
BEZEL_TEST(scoped_builtin_1420, subscribe_method_reference_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Threading; "
        "class Button "
        "  public event Clicked: int; "
        "end Button; "
        "class Prog "
        "  public static method OnClick(n: int) end OnClick; "
        "  public static method Main(): int "
        "    var b := new Button(); "
        "    var sub := b.Clicked.Subscribe(OnClick); "
        "    sub.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §24.3: the flip also covers a value whose static type is the subscribe-only
// interface `IEvent<T>` (the outside-the-declaring-class face).
BEZEL_TEST(scoped_builtin_1420, subscribe_via_ievent_value_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    using var res := new Res(); "
        "    var b := new Button(); "
        "    var ev: IEvent<int> := b.Clicked; "
        "    var sub := ev.Subscribe(lambda(n) => res.Ping()); "
        "    sub.Dispose(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "capture-free"));
}

// ===========================================================================
// AsyncEventSource<T>.Subscribe — async events (§24.3 / stdlib §6.5)
// ===========================================================================

// §28.14 form 2: async-event handler under a `using var` subscription —
// admitted.
BEZEL_TEST(scoped_builtin_1420, async_subscribe_using_var_capture_admitted) {
    auto r = parseAndCheckOwnership(prog(
        "    using var res := new Res(); "
        "    var bc := new Beacon(); "
        "    using var sub := bc.Pulsed.Subscribe(async lambda(n) => do res.Ping(); end); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14: async-event capture-bearing handler on a plain `var` subscription —
// rejected.
BEZEL_TEST(scoped_builtin_1420, async_subscribe_plain_var_capture_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    using var res := new Res(); "
        "    var bc := new Beacon(); "
        "    var sub := bc.Pulsed.Subscribe(async lambda(n) => do res.Ping(); end); "
        "    sub.Dispose(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "capture-free"));
}

// ===========================================================================
// CancellationToken.Register (stdlib §6.1)
// ===========================================================================

// §28.14 form 2: a capture-bearing Register callback under a `using var`
// subscription — admitted.
BEZEL_TEST(scoped_builtin_1420, register_using_var_capture_admitted) {
    auto r = parseAndCheckOwnership(prog(
        "    using var res := new Res(); "
        "    using var cts := new CancellationTokenSource(); "
        "    using var sub := cts.Token.Register(lambda() => res.Ping()); "
        "    cts.Cancel(); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14: a capture-bearing Register callback on a plain `var` subscription —
// rejected.
BEZEL_TEST(scoped_builtin_1420, register_plain_var_capture_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    using var res := new Res(); "
        "    using var cts := new CancellationTokenSource(); "
        "    var sub := cts.Token.Register(lambda() => res.Ping()); "
        "    sub.Dispose(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "capture-free"));
}

// §28.14: a capture-free Register callback needs no bound.
BEZEL_TEST(scoped_builtin_1420, register_capture_free_admitted) {
    auto r = parseAndCheckOwnership(prog(
        "    using var cts := new CancellationTokenSource(); "
        "    var sub := cts.Token.Register(lambda() => ()); "
        "    sub.Dispose(); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Verifiable function-value locals (§28.14 verifiable subset, #1420 triage)
// ===========================================================================

// §28.14: a local bound to a lambda LITERAL and never reassigned is a
// verifiable function value — its captures are the literal's captures. The
// capture-bearing case is admitted under the using-var bound (the #841
// bound-local shape).
BEZEL_TEST(scoped_builtin_1420, subscribe_lambda_literal_local_admitted) {
    auto r = parseAndCheckOwnership(prog(
        "    using var res := new Res(); "
        "    var b := new Button(); "
        "    var h := lambda(n: int) => res.Ping(); "
        "    using var sub := b.Clicked.Subscribe(h); "
        "    b.Fire(); "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14: the same bound-local form WITHOUT a bounding using var is rejected
// via the enumerated captures (not blanket-rejected as unverifiable).
BEZEL_TEST(scoped_builtin_1420,
           subscribe_lambda_literal_local_needs_bound) {
    auto r = parseAndCheckOwnership(prog(
        "    using var res := new Res(); "
        "    var b := new Button(); "
        "    var h := lambda(n: int) => res.Ping(); "
        "    var sub := b.Clicked.Subscribe(h); "
        "    sub.Dispose(); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "capture-free"));
}

// §28.14: a REASSIGNED function-value local stays unverifiable (the binding
// may have been swapped before the call site — flow-insensitive pre-scan).
BEZEL_TEST(scoped_builtin_1420,
           subscribe_reassigned_function_local_rejected) {
    auto r = parseAndCheckOwnership(prog(
        "    var b := new Button(); "
        "    var h := lambda(n: int) => (); "
        "    h := lambda(n: int) => (); "
        "    using var sub := b.Clicked.Subscribe(h); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot be verified"));
}

// ===========================================================================
// The Add (`+=`) form is `scoped` and admits a `this`-capture (§24.3, v0.180)
// ===========================================================================

// §24.3 / §24.5 / §28.14 form 4 (v0.180, #1541): `event += <handler>` inside
// the declaring class IS subject to the §28.14 admission, bounded by the
// publisher `this`. A handler that captures only `this` (here it mutates the
// declaring class's own field) is admitted. (A capture of a shorter-lived
// local/param is rejected — see event_add_scoped_1541_test.)
BEZEL_TEST(scoped_builtin_1420, event_add_this_capture_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Threading; "
        "class Counter "
        "  public event Ticked: int; "
        "  private _total: int; "
        "  public method Wire() "
        "    Ticked += lambda(n) => do _total := _total + n; end; "
        "  end Wire; "
        "end Counter;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
