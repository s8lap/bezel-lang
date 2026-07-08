// Ownership tests for §24.3 / §24.4 / §28.14 (v0.180, #1541) — event capture
// admission: `EventSource<T>.Add` (the `+=` desugaring, and the explicit
// `<event>.Add(h)` spelling) is `scoped`, so the stored handler is admitted
// under §28.14's storage-bounded capture, with the bounding object resolved by
// forms 3 (owned-return handle → `owned` field of `this`) and 4 (a `this`-
// rooted or local-rooted owned/event-backing member chain → the chain head).
//
// §24.3 (v0.180): "`Add` is a `scoped` (storing) position … the bounding object
// is the event's containing instance (form 4 — the receiver is the compiler-
// generated backing member on the publisher). `this.Clicked += OnButtonClicked`
// therefore admits the `this`-capturing method reference … and a handler
// capturing a shorter-lived local than its publisher is rejected at the
// subscription site."
//
// §24.4: the `+=`/`Add` producer-face forms are available only INSIDE the
// declaring class, where the event's static type is `EventSource<T>` — so the
// local-chain-head form 4 (`pub.Clicked += h` with `pub` a same-class local) is
// exercised from a method of the declaring class, not from an external caller
// (externally the event's static type is `IEvent<T>`, which has no `Add`).
//
// §28.14 forms are tested in order (first match wins): form 2 (owned-return
// initializer of a `using var`) wins over form 4 (chain head); the `this` bound
// (forms 3/4) rejects captures of locals/params other than `this`.

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

// A class that subscribes to its OWN event from inside a method — `hookBody` is
// the method body (has access to `this`, its own field `V`, the event, and a
// disposable `Sink` type it may bind to a `using var`).
std::string selfProg(const std::string& hookBody) {
    return std::string(
        "module M; "
        "record Ev(N: int); "
        "class Sink "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "  destructor end; "
        "end Sink; "
        "class Widget "
        "  public V: int := 0; "
        "  public event Clicked: Ev; "
        "  private method OnClick(e: Ev) V := V + e.N; end OnClick; "
        "  public method Hook() ") + hookBody +
        "  end Hook; "
        "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
        "end Widget; "
        "class Prog public static method Main(): int return 0; end Main; "
        "end Prog;";
}

// A publisher class whose OWN static method subscribes through a same-class
// local publisher (`pub: Pub`) — inside the declaring class `pub.Clicked` has
// the `EventSource<Ev>` producer face, so `+=` is available (§24.3/§24.4).
// `body` is spliced into `Pub.Test`.
std::string pubLocalProg(const std::string& body) {
    return std::string(
        "module M; "
        "record Ev(N: int); "
        "class Sink "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "  destructor end; "
        "end Sink; "
        "class Pub "
        "  public event Clicked: Ev; "
        "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
        "  public static method Test(): int ") + body +
        "  end Test; "
        "end Pub;";
}

}  // namespace

// ===========================================================================
// §28.14 form 4 — bounding object is `this` (the publisher)
// ===========================================================================

// §24.3 / §28.14 form 4: `Clicked += OnClick` is a bare method reference
// capturing only `this` (self-capture of the bounding object) — ADMITTED.
BEZEL_TEST(event_add_scoped_1541, self_capture_named_method_ref_admitted) {
    auto r = parseAndCheckOwnership(selfProg("    Clicked += OnClick; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 form 4: a `+=` lambda that touches only the declaring class's own
// field (implicit `this`) is ADMITTED.
BEZEL_TEST(event_add_scoped_1541, self_capture_lambda_this_field_admitted) {
    auto r = parseAndCheckOwnership(
        selfProg("    Clicked += (e) => do V := V + e.N; end; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 form 4 (the #1541 p15 UAF): a `+=` handler capturing a method-scoped
// `using var` — shorter-lived than the publisher `this` — is REJECTED at the
// subscription site (a later raise would use it after disposal).
BEZEL_TEST(event_add_scoped_1541, plus_equals_captures_using_var_rejected) {
    auto r = parseAndCheckOwnership(selfProg(
        "    using var s := new Sink(); "
        "    Clicked += (e) => do s.Add(e.N); end; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "`s`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "is not `this`"));
}

// §28.14 form 4: the explicit `this.Clicked.Add(handler)` spelling enforces the
// same admission — capturing a shorter-lived local is REJECTED.
BEZEL_TEST(event_add_scoped_1541, explicit_add_captures_using_var_rejected) {
    auto r = parseAndCheckOwnership(selfProg(
        "    using var s := new Sink(); "
        "    this.Clicked.Add((e) => do s.Add(e.N); end); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "is not `this`"));
}

// §28.14 form 4: a captured method/constructor parameter (a borrow of a caller
// local) does not provably outlive `this`, so it too is REJECTED. This is the
// pre-v0.180 codegen shape (`Hook(target: Sink)` storing a `target`-capturing
// handler via `+=`) that compiled and would SEGFAULT on a post-disposal raise.
BEZEL_TEST(event_add_scoped_1541, plus_equals_captures_parameter_rejected) {
    auto r = parseAndCheckOwnership(std::string(
        "module M; "
        "record Ev(N: int); "
        "class Sink public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; end Sink; "
        "class Widget "
        "  public event Clicked: Ev; "
        "  public method Hook(target: Sink) "
        "    Clicked += (e) => do target.Add(e.N); end; "
        "  end Hook; "
        "end Widget; "
        "class Prog public static method Main(): int return 0; end Main; "
        "end Prog;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "`target`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "is not `this`"));
}

// ===========================================================================
// §28.14 form 4 — chain head is a `using var` local publisher (inside the
// declaring class, where `pub.Clicked` has the `EventSource<T>` face)
// ===========================================================================

// §28.14 form 4 (local chain head): `pub.Clicked += handler` bounds by the
// publisher local `pub`; a source declared BEFORE `pub` (so it outlives `pub`
// under reverse-declaration-order disposal) is ADMITTED.
BEZEL_TEST(event_add_scoped_1541, form4_local_head_source_before_admitted) {
    auto r = parseAndCheckOwnership(pubLocalProg(
        "    using var early := new Sink(); "
        "    using var pub := new Pub(); "
        "    pub.Clicked += (e) => do early.Add(e.N); end; "
        "    return 0; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 form 4 (local chain head): a source declared AFTER the publisher local
// dies before the registration — REJECTED, naming the declared-before rule.
BEZEL_TEST(event_add_scoped_1541, form4_local_head_source_after_rejected) {
    auto r = parseAndCheckOwnership(pubLocalProg(
        "    using var pub := new Pub(); "
        "    using var late := new Sink(); "
        "    pub.Clicked += (e) => do late.Add(e.N); end; "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "`late`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "declared before"));
}

// ===========================================================================
// §28.14 form 3 — owned-return handle stored in an `owned` field of `this`
// ===========================================================================

// §24.4 / §28.14 form 3: the observer pattern — `Subscribe`'s owned
// `Subscription` assigned to an `owned` field of `this` resolves the bounding
// object to `this`, admitting a handler that captures `this` (the spec's
// TickObserver example).
BEZEL_TEST(event_add_scoped_1541, form3_owned_subscription_field_admits_this) {
    auto r = parseAndCheckOwnership(std::string(
        "module M; "
        "record Tick(N: int); "
        "class Observer "
        "  private owned _sub: Subscription; "
        "  private _count: int := 0; "
        "  public constructor(ticks: IEvent<Tick>) "
        "    _sub := ticks.Subscribe((e) => do OnTick(e); end); "
        "  end constructor; "
        "  private method OnTick(e: Tick) _count := _count + 1; end OnTick; "
        "end Observer; "
        "class Prog public static method Main(): int return 0; end Main; "
        "end Prog;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// §28.14 ordering — form 2 wins over form 4
// ===========================================================================

// §28.14: "using var sub := pub.Clicked.Subscribe(h) bounds by sub (form 2, the
// tightest bound), not by pub (form 4)." A source declared before `sub` (but
// after `pub`) is admitted — proving the bound is `sub`, not `pub`.
BEZEL_TEST(event_add_scoped_1541, form2_subscribe_using_var_wins_over_form4) {
    auto r = parseAndCheckOwnership(std::string(
        "module M; "
        "record Ev(N: int); "
        "class Sink public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; destructor end; "
        "end Sink; "
        "class Pub public event Clicked: Ev; "
        "  public method Fire(n: int) Clicked.Raise(new Ev(n)); end Fire; "
        "end Pub; "
        "class Prog public static method Main(): int "
        "    using var pub := new Pub(); "
        "    using var early := new Sink(); "
        "    using var sub := "
        "        pub.Clicked.Subscribe((e) => do early.Add(e.N); end); "
        "    return 0; "
        "  end Main; "
        "end Prog;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
