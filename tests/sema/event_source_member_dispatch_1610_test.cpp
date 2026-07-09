// fix #1610 — §6.5 / §24.3: member-call dispatch for `EventSource<T>` /
// `AsyncEventSource<T>` values held in a local or parameter.
//
// Before #1610, ANY method call on an `EventSource<T>`/`AsyncEventSource<T>`-
// typed local/parameter — valid or bogus — silently compiled with no
// diagnostic and no effect: these builtin generics (declaration == nullptr)
// had no `rcn` member-dispatch block in `ExprTypecheck_Calls.cpp` (the
// `Channel<T>`/`Weak<T>` siblings each have one), so member lookup never
// resolved their methods and no unrecognized-name check fired. The companion
// codegen test (event_source_var_1610_test.cpp) pins the runtime no-op; THIS
// test pins the sema facet — that a bogus member is now a compile error and a
// real member is accepted.
//
// Spec authority:
//   - §6.5 / §24.3 (member access): a call on a value dispatches to a real
//     member; an unrecognized member name is a compile error, not a silent
//     no-op.
//   - stdlib §6.5: `EventSource<T>` exposes Subscribe/Raise/Add/Remove;
//     `AsyncEventSource<T>` exposes Subscribe/RaiseAsync/Add/Remove.
//
// Each case FAILS without the fix (bogus member accepted / no error emitted)
// and PASSES with it.

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
// (1) §6.5 / §24.3 / #1610: a BOGUS member on an `EventSource<T>` parameter is
//     now rejected (before the fix it was silently accepted). The diagnostic
//     names the type and the documented surface.
// ---------------------------------------------------------------------------
BEZEL_TEST(event_source_member_dispatch_1610, bogus_member_rejected) {
    auto r = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "class Ev public N: int := 0; end Ev; "
        "public class Counter "
        "  public event Ticked: Ev; "
        "  public method Fire(es: EventSource<Ev>) "
        "    es.TotallyBogusMethodThatDoesNotExist(42); "
        "  end Fire; "
        "end Counter;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiag(r, "no member `TotallyBogusMethodThatDoesNotExist`"));
    BEZEL_EXPECT_TRUE(hasDiag(r, "EventSource<T>"));
}

// ---------------------------------------------------------------------------
// (2) §6.5 / §24.3 / #1610: a REAL member call on an `EventSource<T>`
//     parameter compiles cleanly — Raise / Subscribe / Add / Remove are all on
//     the producer surface (stdlib §6.5). (Before the fix these compiled too,
//     but only because EVERYTHING compiled; this guards against an over-broad
//     rejection accidentally forbidding a valid method.)
// ---------------------------------------------------------------------------
BEZEL_TEST(event_source_member_dispatch_1610, real_members_accepted) {
    auto r = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "class Ev public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; end Ev; "
        "public class Counter "
        "  public event Ticked: Ev; "
        "  public method Fire(es: EventSource<Ev>) "
        "    es.Raise(new Ev(1)); "
        "    es.Add((e: Ev) => do end); "
        "    es.Remove((e: Ev) => do end); "
        "  end Fire; "
        "  public method Sub(es: EventSource<Ev>) "
        "    using var s := es.Subscribe((e: Ev) => do end); "
        "  end Sub; "
        "end Counter;");
    BEZEL_EXPECT_FALSE(hasDiag(r, "no member `"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (3) §6.5 / §24.3 / #1610: the async sibling — a bogus member on an
// `AsyncEventSource<T>` parameter is rejected.
// ---------------------------------------------------------------------------
BEZEL_TEST(event_source_member_dispatch_1610, async_bogus_member_rejected) {
    auto r = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "class Ev public N: int := 0; end Ev; "
        "public class Counter "
        "  public async event Ticked: Ev; "
        "  public method Fire(es: AsyncEventSource<Ev>) "
        "    es.NoSuchAsyncOp(); "
        "  end Fire; "
        "end Counter;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiag(r, "no member `NoSuchAsyncOp`"));
    BEZEL_EXPECT_TRUE(hasDiag(r, "AsyncEventSource<T>"));
}

// ---------------------------------------------------------------------------
// (4) §6.5 / #1610: the producer surfaces are DISTINCT — `Raise` lives on
//     `EventSource<T>` only and `RaiseAsync` on `AsyncEventSource<T>` only
//     (stdlib §6.5). `Raise` on an `AsyncEventSource<T>` is rejected, and
//     `RaiseAsync` on an `EventSource<T>` is rejected.
// ---------------------------------------------------------------------------
BEZEL_TEST(event_source_member_dispatch_1610, raise_vs_raiseasync_distinct) {
    // Raise on AsyncEventSource<T> — rejected.
    auto ra = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "class Ev public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; end Ev; "
        "public class Counter "
        "  public async event Ticked: Ev; "
        "  public method Fire(es: AsyncEventSource<Ev>) es.Raise(new Ev(1)); end Fire; "
        "end Counter;");
    BEZEL_EXPECT_TRUE(ra.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiag(ra, "no member `Raise`"));

    // RaiseAsync on EventSource<T> — rejected.
    auto rb = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "class Ev public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; end Ev; "
        "public class Counter "
        "  public event Ticked: Ev; "
        "  public method Fire(es: EventSource<Ev>) es.RaiseAsync(new Ev(1)); end Fire; "
        "end Counter;");
    BEZEL_EXPECT_TRUE(rb.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiag(rb, "no member `RaiseAsync`"));
}

// ---------------------------------------------------------------------------
// (5) §6.5 / §24.3 / #1610: the LOCAL-variable form (issue repro variant 2:
//     `var es: EventSource<T> := Ticked; es.<bogus>(...)`) is rejected too —
//     not only the parameter form.
// ---------------------------------------------------------------------------
BEZEL_TEST(event_source_member_dispatch_1610, local_bogus_member_rejected) {
    auto r = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "class Ev public N: int := 0; end Ev; "
        "public class Counter "
        "  public event Ticked: Ev; "
        "  public method Tick() "
        "    var es: EventSource<Ev> := Ticked; es.NotARealMember(); "
        "  end Tick; "
        "end Counter;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiag(r, "no member `NotARealMember`"));
}
