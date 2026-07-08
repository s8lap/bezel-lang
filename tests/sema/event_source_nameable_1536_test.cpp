// fix #1536 — stdlib §6.5 / spec §2.3 / §24.3: `EventSource<T>` and
// `AsyncEventSource<T>` were not registered as builtin types at all, so a
// source-level reference to either name (e.g. a helper method's parameter
// type) failed to resolve with "unresolved type name `EventSource`" even
// after `import System.Threading;`.
//
// Spec authority:
//   - Spec §2.3 ("Always-available names"): "`IEvent<T>` and `IAsyncEvent<T>`
//     are auto-imported ... Importing `System.Threading` is not required to
//     use `Subscription`, `CancellationToken`, `IEvent<T>`, or
//     `IAsyncEvent<T>`; it is required for `CancellationTokenSource`,
//     `Mutex`, `EventSource<T>`, `AsyncEventSource<T>`, and the other
//     threading types."
//   - Stdlib §6.5 ("Events: IEvent, EventSource"): declares
//     `EventSource<T>: IEvent<T>` and `AsyncEventSource<T>: IAsyncEvent<T>`
//     in the `System.Threading` module; "Inside the declaring class, the
//     static type is `EventSource<T>` (or `AsyncEventSource<T>`), where
//     `Add`, `Remove`, `Raise`, and `Subscribe` are all available" — so a
//     private helper taking that type as a parameter is spec-legal (§24.3).
//
// These tests verify only the name-resolution gap the issue describes: that
// `EventSource<T>` / `AsyncEventSource<T>` resolve (as a parameter type) once
// `import System.Threading;` is present, and that the generic-arity check
// applies to them like every other builtin generic type (stdlib §6.9's
// Channel<T> is the sibling precedent for this arity-check pattern).

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
// (1) §2.3 / §6.5 / #1536: `EventSource<T>` names cleanly as a parameter type
//     after `import System.Threading;` — the issue's own repro shape (a
//     helper method on the declaring class taking the in-class producer-side
//     face as a parameter, §24.3).
// ---------------------------------------------------------------------------
BEZEL_TEST(event_source_nameable_1536, event_source_resolves_with_import) {
    auto r = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "record TickEvent(N: int); "
        "public class Counter "
        "  public event Ticked: TickEvent; "
        "  public method Helper(es: EventSource<TickEvent>) "
        "  end Helper; "
        "end Counter;");
    BEZEL_EXPECT_FALSE(hasDiag(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (2) §2.3 / §6.5 / #1536: `AsyncEventSource<T>` — the async sibling — names
//     the same way.
// ---------------------------------------------------------------------------
BEZEL_TEST(event_source_nameable_1536, async_event_source_resolves_with_import) {
    auto r = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "record TickEvent(N: int); "
        "public class Counter "
        "  public async event Ticked: TickEvent; "
        "  public method Helper(es: AsyncEventSource<TickEvent>) "
        "  end Helper; "
        "end Counter;");
    BEZEL_EXPECT_FALSE(hasDiag(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (3) §2.3 / §6.5 / #1536: the no-payload event form — `EventSource<()>`
//     (stdlib §6.5: "For no-payload events ... the payload is `()`") —
//     resolves too, exercising the unit-type generic argument.
// ---------------------------------------------------------------------------
BEZEL_TEST(event_source_nameable_1536, event_source_unit_payload_resolves) {
    auto r = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "public class Counter "
        "  public event Done; "
        "  public method Helper(es: EventSource<()>) "
        "  end Helper; "
        "end Counter;");
    BEZEL_EXPECT_FALSE(hasDiag(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (4) Arity check: `EventSource` used with no generic argument is diagnosed
//     as a wrong-arity built-in reference (same class of diagnostic as every
//     other builtin generic type, e.g. Channel<T>) rather than silently
//     accepted or crashing.
// ---------------------------------------------------------------------------
BEZEL_TEST(event_source_nameable_1536, event_source_missing_arity_diagnosed) {
    auto r = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "public class Counter "
        "  public method Helper(es: EventSource) "
        "  end Helper; "
        "end Counter;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiag(r, "EventSource"));
}

// ---------------------------------------------------------------------------
// (5) Arity check: `AsyncEventSource` with too many generic arguments is
//     likewise diagnosed.
// ---------------------------------------------------------------------------
BEZEL_TEST(event_source_nameable_1536, async_event_source_extra_arity_diagnosed) {
    auto r = parseSemaAll(
        "module Probe; "
        "import System.Threading; "
        "record TickEvent(N: int); "
        "public class Counter "
        "  public method Helper(es: AsyncEventSource<TickEvent, TickEvent>) "
        "  end Helper; "
        "end Counter;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiag(r, "AsyncEventSource"));
}
