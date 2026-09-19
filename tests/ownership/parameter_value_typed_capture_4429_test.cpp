// Ownership regression test for #4429 — §28.14 (v0.301) capture reduction:
// the `ParameterSymbol` branch in `Ownership_Checks.cpp` lacks the
// `isBorrowableType` filter the `LocalSymbol` branch has.
//
// §28.3: "a value-typed capture contributes nothing, living in its heap
// capture cell". §28.14 keys the `this` admission arm on "every captured
// source" (which is §28.3's term), so a captured value-typed parameter
// satisfies it vacuously — exactly like a captured value-typed local.
//
// Pre-fix the ParameterSymbol branch unconditionally added the capture to
// `nonThisCaptures`, which the `this` admission arm then rejected with
// "captured source `name` is not `this`", even though `name` is `string`.
// The LocalSymbol branch, by contrast, dropped value-typed captures BEFORE
// adding them to `nonThisCaptures`. The asymmetry is the bug.
//
// The fix: mirror the LocalSymbol branch's filter on the ParameterSymbol
// branch — if the parameter's declared type is not borrowable (per
// `isBorrowableType`), drop the capture before it can reach the
// non-this-captures classification. Null declType stays conservative
// (treated as a reference) so an unresolved type still flows into the
// downstream check unchanged.

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

// Fixture: IEvent<int>-typed ticks parameter, owned-field Subscription, so the
// subscription lands via §28.14 form 3 (owned field of `this`). `body` is
// spliced in as the Ticker constructor body. Matches the issue's exact probe
// shape — the asymmetry under test.
std::string ctorProg(const std::string& body) {
    return std::string(
        "module M; "
        "class Ticker "
        "  private owned _sub: Subscription; "
        "  public constructor(ticks: IEvent<int>, name: string) ") + body +
        "  end constructor; "
        "end Ticker; "
        "class Prog "
        "  public static method Main(): int "
        "    using var src := new EventSource<int>(); "
        "    var ev: IEvent<int> := src; "
        "    using var t := new Ticker(ev, \"hello\"); "
        "    return 0; "
        "  end Main; "
        "end Prog;";
}

}  // namespace

// §28.14 v0.301 (#4429): a captured VALUE-TYPED parameter contributes no
// borrow source — the §28.3 "value-typed capture contributes nothing"
// ruling. Pre-fix the ParameterSymbol branch unconditionally added it to
// `nonThisCaptures`, which the `this` admission arm rejected with
// "captured source `name` is not `this`". Post-fix the capture is dropped
// before it can reach that arm.
BEZEL_TEST(parameter_value_typed_capture_4429, string_param_capture_admitted) {
    auto r = parseAndCheckOwnership(ctorProg(
        "    _sub := ticks.Subscribe(lambda(n) => Console.WriteLine(name)); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 v0.301 (#4429): the LocalSymbol branch already had the filter, so
// a captured value-typed local works correctly today and continues to work
// post-fix (control — the asymmetry the fix closes is that the local
// branch was right and the param branch was wrong).
BEZEL_TEST(parameter_value_typed_capture_4429, string_local_capture_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Publisher "
        "  public event Clicked: int; "
        "  public method Fire(n: int) Clicked.Raise(n); end Fire; "
        "end Publisher; "
        "class Prog "
        "  public static method Main(): int "
        "    var name := \"hello\"; "
        "    var p := new Publisher(); "
        "    using var sub := p.Clicked.Subscribe(lambda(n) => Console.WriteLine(name)); "
        "    p.Fire(1); "
        "    sub.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 v0.301 (#4429): a captured CLASS-TYPED parameter is still
// rejected — the filter only DROPS value-typed captures, it does not
// weaken class/interface capture admission. (This is what `l6_5_0018`
// also pins; we pin it here as a control alongside the fix's own probe.)
BEZEL_TEST(parameter_value_typed_capture_4429, class_param_capture_rejected) {
    // Logger is a class; `logger` is a parameter. The lambda captures
    // `logger`, which the fix's filter does NOT touch (a class reference
    // is borrowable), so it remains a non-this-capture and the §28.14
    // this-admission arm fires "captured source `logger` is not `this`".
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Logger "
        "  public constructor() end constructor; "
        "  public method Write(n: int) Console.WriteLine(n); end Write; "
        "end Logger; "
        "class TickObserver "
        "  private owned _sub: Subscription; "
        "  public constructor(ticks: IEvent<int>, logger: Logger) "
        "    _sub := ticks.Subscribe(lambda(n) => logger.Write(n)); "
        "  end constructor; "
        "end TickObserver; "
        "class Prog "
        "  public static method Main(): int "
        "    using var src := new EventSource<int>(); "
        "    var ev: IEvent<int> := src; "
        "    using var obs := new TickObserver(ev, new Logger()); "
        "    return 0; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "captured source `logger`"));
}

// §28.14 v0.301 (#4429): an `int` parameter — also a primitive value type
// — is captured cleanly under the same path as `string`. (Mirrors the
// LocalSymbol branch's value-typed-admit control already in
// `scoped_builtin_subscribe_1420_test.cpp`, but for a parameter rather
// than a local — the exact asymmetry the fix closes.)
BEZEL_TEST(parameter_value_typed_capture_4429, int_param_capture_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Ticker "
        "  private owned _sub: Subscription; "
        "  public constructor(ticks: IEvent<int>, count: int) "
        "    _sub := ticks.Subscribe(lambda(n) => Console.WriteLine(count + n)); "
        "  end constructor; "
        "end Ticker; "
        "class Prog "
        "  public static method Main(): int "
        "    using var src := new EventSource<int>(); "
        "    var ev: IEvent<int> := src; "
        "    using var t := new Ticker(ev, 0); "
        "    return 0; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}