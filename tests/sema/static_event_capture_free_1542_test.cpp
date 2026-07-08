// Spec-derived sema tests for §22.12 rule 5 (v0.180, #1542): a handler
// subscribed to a `static event` must be capture-free.
//
// §22.12 rule 5: "Static events are runtime-synchronized and capture-free
// ... a handler subscribed to a `static event` — through `+=`/`Add` or
// `Subscribe` — must be capture-free: a non-capturing lambda or a static
// method reference, the same admission as `Domain.Spawn` entries (§30.14)
// and FFI callbacks. The process-global subscriber list transports the
// handler to whichever domain raises (§24.5: handlers execute on the raising
// domain), and a capture-bearing closure carries borrows of its origin
// domain's scopes ... a capture-bearing subscription to a static event is a
// compile error naming this rule. ... (Instance events remain unsynchronized
// and capture-admitting ...)"
//
// This mirrors the existing §30.14 Domain.Spawn capture-free classifier
// (isCaptureFreeEntry, tests/sema/send_600_spawn_callsite_test.cpp): a lambda
// that references only static members (no local/parameter/`this`) has an
// empty capture set and is therefore capture-free, exactly like a lambda
// entry passed to Domain.Spawn.

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

// True iff a diagnostic names §22.12 rule 5's capture-free requirement.
bool hasStaticEventCaptureDiag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("static event") != std::string::npos &&
            d.message.find("capture-free") != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Rejected: a capturing handler subscribed to a static event.
// ---------------------------------------------------------------------------

// §22.12 rule 5: `<StaticEvent>.Subscribe(handler)` where the handler lambda
// captures a local variable of the enclosing method → rejected.
BEZEL_TEST(static_event_capture_free_1542,
           subscribe_capturing_local_rejected_22_12_5) {
    auto r = parseSemaAll(
        "module M; "
        "record Ev(N: int); "
        "class Bus "
        "  public static event Ticked: Ev; "
        "end Bus; "
        "class C "
        "  public method Use() "
        "    var total := 0; "
        "    using var sub := Bus.Ticked.Subscribe((e) => do total := total + e.N; end); "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasStaticEventCaptureDiag(r));
}

// §22.12 rule 5: `<StaticEvent>.Add(handler)` (inside the declaring class,
// where Add is on the EventSource<T> face, §24.3) with a capturing handler
// → rejected the same way as Subscribe.
BEZEL_TEST(static_event_capture_free_1542,
           add_capturing_local_rejected_22_12_5) {
    auto r = parseSemaAll(
        "module M; "
        "record Ev(N: int); "
        "class Bus "
        "  public static event Ticked: Ev; "
        "  public static method Hook(offset: int) "
        "    Ticked.Add((e) => do offset := offset + e.N; end); "
        "  end Hook; "
        "end Bus;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasStaticEventCaptureDiag(r));
}

// §22.12 rule 5 / §24.4: `staticEvent += handler` desugars to Add — a
// capturing handler (captures the enclosing static method's parameter
// `offset`) is rejected exactly as the explicit `.Add(...)` form is.
BEZEL_TEST(static_event_capture_free_1542,
           plus_equals_capturing_parameter_rejected_22_12_5) {
    auto r = parseSemaAll(
        "module M; "
        "record Ev(N: int); "
        "class Bus "
        "  public static event Signal: Ev; "
        "  public static method Fire(n: int) Signal.Raise(new Ev(n)); end Fire; "
        "  public static method Hook(offset: int) "
        "    Signal += (e) => do Bus.Fire(e.N + offset); end; "
        "  end Hook; "
        "end Bus;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasStaticEventCaptureDiag(r));
}

// ---------------------------------------------------------------------------
// Accepted: capture-free handlers subscribed to a static event.
// ---------------------------------------------------------------------------

// §22.12 rule 5: a non-capturing lambda that touches only a STATIC field (no
// local, no parameter, no `this`) has an empty capture set and is accepted —
// "the same admission as Domain.Spawn entries" (§30.14's isCaptureFreeEntry
// treats a static-field reference identically: not a capture).
BEZEL_TEST(static_event_capture_free_1542,
           plus_equals_noncapturing_lambda_over_static_field_accepted_22_12_5) {
    auto r = parseSemaAll(
        "module M; "
        "record Ev(N: int); "
        "class Bus "
        "  public static Total: int := 0; "
        "  public static event Signal: Ev; "
        "  public static method Hook() "
        "    Signal += (e) => do Bus.Total := Bus.Total + e.N; end; "
        "  end Hook; "
        "end Bus;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.12 rule 5: a static method reference (qualified `Type.Method`) as the
// `Subscribe` handler → accepted, the same admission as a Domain.Spawn entry.
BEZEL_TEST(static_event_capture_free_1542,
           subscribe_static_method_ref_accepted_22_12_5) {
    auto r = parseSemaAll(
        "module M; "
        "record Ev(N: int); "
        "class Bus "
        "  public static event Ticked: Ev; "
        "  public static method OnTick(e: Ev) end OnTick; "
        "end Bus; "
        "class C "
        "  public method Use() "
        "    using var sub := Bus.Ticked.Subscribe(Bus.OnTick); "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.12 rule 5: an unqualified static method reference (bare identifier
// resolving to a static method of the enclosing class, fix #1492's spelling)
// as the `+=` handler → accepted.
BEZEL_TEST(static_event_capture_free_1542,
           plus_equals_unqualified_static_method_ref_accepted_22_12_5) {
    auto r = parseSemaAll(
        "module M; "
        "record Ev(N: int); "
        "class Bus "
        "  public static event Signal: Ev; "
        "  public static method OnSignal(e: Ev) end OnSignal; "
        "  public static method Hook() "
        "    Signal += OnSignal; "
        "  end Hook; "
        "end Bus;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Control: instance events remain capture-admitting (no over-rejection).
// ---------------------------------------------------------------------------

// §22.12 rule 5's last sentence: "Instance events remain unsynchronized and
// capture-admitting." A capturing handler subscribed to a non-static
// (instance) event must NOT be rejected by this rule.
BEZEL_TEST(static_event_capture_free_1542,
           instance_event_subscribe_capturing_local_still_accepted_22_12_5) {
    auto r = parseSemaAll(
        "module M; "
        "record Ev(N: int); "
        "class Src "
        "  public event Ticked: Ev; "
        "end Src; "
        "class C "
        "  public method Use() "
        "    var total := 0; "
        "    using var src := new Src(); "
        "    using var sub := src.Ticked.Subscribe((e) => do total := total + e.N; end); "
        "  end Use; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.12 rule 5's last sentence: `instanceEvent += handler` with a capturing
// handler (inside the declaring class, on the EventSource<T> face) must also
// remain accepted — the rule targets `static event` only.
BEZEL_TEST(static_event_capture_free_1542,
           instance_event_plus_equals_capturing_field_still_accepted_22_12_5) {
    auto r = parseSemaAll(
        "module M; "
        "record Ev(N: int); "
        "class Src "
        "  public Total: int := 0; "
        "  public event Signal: Ev; "
        "  public method Hook() "
        "    Signal += (e) => do Total := Total + e.N; end; "
        "  end Hook; "
        "end Src;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
