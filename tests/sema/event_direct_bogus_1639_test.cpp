// fix #1639 — stdlib §6.5 / language §24.3: a bogus method call on a DIRECT
// event-member access (`Clicked.Bogus()`, where `Clicked` names the event
// itself rather than a local/parameter/field typed `EventSource<T>`) silently
// compiled and no-op'd instead of being rejected.
//
// #1610 (commit 0bfa06d / event_source_member_dispatch_1610_test.cpp) fixed
// the same silent no-op for an `EventSource<T>`/`AsyncEventSource<T>`-TYPED
// local, parameter, or field — a receiver whose `exprType` is set, and which
// therefore reaches the `rcn == "EventSource"` member-dispatch block in
// ExprTypecheck_Calls.cpp. #1610 explicitly left the DIRECT access path out
// of scope: "Direct <event>.Method() access is unchanged — its receiver
// carries no exprType, so it never reaches this block and keeps dispatching
// via the EventSymbol-keyed path." That EventSymbol-keyed path
// (resolveMemberAccess in ExprTypecheck_Calls.cpp) validated only the
// producer/subscriber VISIBILITY fence (§24.3) for the four operation names
// it already knew about (Raise/RaiseAsync/Add/Remove) — it never checked
// whether `ma.member` was a real event operation at all, so an unrecognized
// name (any name outside the whole documented surface) fell through to
// ordinary member lookup, which never resolves an event name either (the
// event has no user TypeSymbol), and both sema and codegen's
// tryLowerEventCall silently no-op'd.
//
// Spec authority:
//   - stdlib §6.5: `EventSource<T>` exposes Subscribe/Raise/Add/Remove (plus
//     a compiler-generated Dispose common to every owned/disposable type);
//     `AsyncEventSource<T>` exposes Subscribe/RaiseAsync/Add/Remove/Dispose.
//   - language §24.3: a direct event-member access dispatches against this
//     documented surface; a name outside it is a compile error, not a silent
//     no-op — the same posture #1610 established for the typed-value case.
//
// Each "rejected" case FAILS (no diagnostic; bogus member silently accepted)
// without the fix and PASSES (clean `no member` diagnostic) with it. Each
// "accepted" case is a control pinning that the fix does not over-reject a
// real member of the documented surface.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiag(const bezel::test::SemaResult& r, std::string_view substr) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(substr) != std::string::npos) return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// (1) §6.5 / §24.3 / #1639: a bogus method on a DIRECT event access, called
//     from inside the declaring class (where the producer-side face is in
//     scope), is now rejected. This is the exact repro from the issue.
// ---------------------------------------------------------------------------
BEZEL_TEST(event_direct_bogus_1639, bogus_member_rejected_direct_access) {
    auto r = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "class Ev public N: int := 0; end Ev; "
        "public class Counter "
        "  public event Ticked: Ev; "
        "  public method Fire() "
        "    Ticked.Bogus(); "
        "  end Fire; "
        "end Counter;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiag(r, "no member `Bogus`"));
    BEZEL_EXPECT_TRUE(hasDiag(r, "event `Ticked`"));
}

// ---------------------------------------------------------------------------
// (2) §6.5 / §24.3 / #1639: the async sibling — a bogus method on a direct
//     `async event` access is rejected too.
// ---------------------------------------------------------------------------
BEZEL_TEST(event_direct_bogus_1639, async_bogus_member_rejected_direct_access) {
    auto r = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "class Ev public N: int := 0; end Ev; "
        "public class Counter "
        "  public async event Ticked: Ev; "
        "  public method Fire() "
        "    Ticked.NoSuchAsyncOp(); "
        "  end Fire; "
        "end Counter;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiag(r, "no member `NoSuchAsyncOp`"));
}

// ---------------------------------------------------------------------------
// (3) §6.5 / §24.3 / #1639: the documented producer-side surface — Raise,
//     Add, Remove, Subscribe, and the compiler-generated Dispose — still
//     compiles clean on a direct access from inside the declaring class.
//     (Control: the fix must not over-reject real members.)
// ---------------------------------------------------------------------------
BEZEL_TEST(event_direct_bogus_1639, real_members_accepted_direct_access) {
    auto r = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "class Ev "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Ev; "
        "public class Counter "
        "  public event Ticked: Ev; "
        "  public method Fire() "
        "    Ticked.Raise(new Ev(1)); "
        "    Ticked.Add((e: Ev) => do end); "
        "    Ticked.Remove((e: Ev) => do end); "
        "    Ticked.Dispose(); "
        "  end Fire; "
        "  public method Sub() "
        "    using var s := Ticked.Subscribe((e: Ev) => do end); "
        "  end Sub; "
        "end Counter;");
    BEZEL_EXPECT_FALSE(hasDiag(r, "no member `"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (4) §24.3 / #1639: `Subscribe` on a direct event access from OUTSIDE the
//     declaring class remains accepted (the subscribe-only `IEvent<T>` face
//     is in scope everywhere) — the new bogus-member check must not
//     interfere with the pre-existing §24.3 visibility posture.
// ---------------------------------------------------------------------------
BEZEL_TEST(event_direct_bogus_1639, subscribe_from_outside_still_accepted) {
    auto r = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "class Ev public N: int := 0; end Ev; "
        "public class Counter "
        "  public event Ticked: Ev; "
        "end Counter; "
        "public class Outside "
        "  public method Watch(c: Counter) "
        "    using var sub := c.Ticked.Subscribe((e: Ev) => do end); "
        "  end Watch; "
        "end Outside;");
    BEZEL_EXPECT_FALSE(hasDiag(r, "no member `"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (5) §24.3 / #1639: a bogus member from OUTSIDE the declaring class is
//     rejected by the new check (not silently accepted just because the
//     visibility-fence check at that call site is gated on a fixed list of
//     recognized names and does not itself fire for an unrecognized one).
// ---------------------------------------------------------------------------
BEZEL_TEST(event_direct_bogus_1639, bogus_member_rejected_from_outside) {
    auto r = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "class Ev public N: int := 0; end Ev; "
        "public class Counter "
        "  public event Ticked: Ev; "
        "end Counter; "
        "public class Outside "
        "  public method Poke(c: Counter) "
        "    c.Ticked.TotallyBogus(); "
        "  end Poke; "
        "end Outside;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiag(r, "no member `TotallyBogus`"));
}
