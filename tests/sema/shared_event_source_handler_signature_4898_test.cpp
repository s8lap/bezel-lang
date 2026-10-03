// fix #4898 (stdlib §6.5; spec §27.1.1/§27.6, v0.307): the handler slot on
// `ISharedEvent<T>.Subscribe` / `SharedEventSource<T>.{Subscribe, Add,
// Remove}` direct calls was not checked against the stdlib-stated `(T) -> ()`
// (capture-free) signature. Pre-fix an arity-2 handler, an `owned` formal,
// a wrong-typed parameter or a value-returning method compiled and codegen
// invoked the handler against the raw payload (the SharedEventSource<T>
// pair's runtime representation is the same `bz_event_source_t*` an
// `EventSource<T>` carries; #4901's codegen gate `sharedEventHandlerAdmissible`
// screens each handler, but the slot shape itself — the `(T) -> ()` identity
// — was not pinned in sema). Only the `+=`/`-=` desugarings (#4869) and the
// plain-event pair (#4968, on `EventSource<T>`/`IEvent<T>` etc.) were
// checked.
//
// The fix extends #4968's `plainEventHandlerSlot` and `eventHandlerPayloadType`
// (sema/ExprTypecheck_Calls.cpp) to recognise `SharedEventSource<T>` (all
// three of Subscribe/Add/Remove) and `ISharedEvent<T>` (Subscribe only —
// the public subscribe-only face stdlib §6.5 declares). The shared pair's
// handler TYPE is the same `(T) -> ()` as the plain pair, so the existing
// `checkEventHandlerArgument` (§27.1.1 parameter arity, modifier, identity
// or class/interface contravariance; §27.6 adapter-bridged formation is the
// same deliberate spurious reject #4978) applies without change. The
// capture-freedom admission itself (#4920) is a separate, orthogonal check
// on the shared pair — both checks fire; an unsound capturing handler is
// rejected by #4920 first (its diagnostic names §6.5/§28.14), an unsound
// shape is rejected by #4898's extended slot check (its diagnostic names
// §6.5/§27.1.1).
//
// T1 — bound method on SharedEventSource.Subscribe: rejects (this is a
// non-static method reference; both #4920's capture-free gate and #4898's
// extended slot check would reject — #4920 fires first).
// T2 — capturing lambda on SharedEventSource.Subscribe: rejects (§6.5
// capture-free admission, #4920's diagnostic).
// T3 — static method reference on SharedEventSource.Subscribe: ACCEPT
// end-to-end (the codegen gate `sharedEventHandlerAdmissible` recognises
// the static method ref and lowers).
// T4 — capture-free lambda on SharedEventSource.Subscribe: sema accepts
// (capture-free, shape `(int) -> ()` matches). Codegen does NOT yet lower
// a non-static lambda on the shared pair (tracked as #4972 — codegen only
// claims a static MethodSymbol today), so this row is a CONTROL pinning
// the codegen #2457 backstop diagnostic — NOT a regression to fix here.
// The test pin is the semantic shape check: a capture-free lambda with
// the correct `(T) -> ()` shape now passes the §6.5 / §27.1.1 slot
// check that pre-fix was missing.
// T5 — bound method on ISharedEvent<int>.Subscribe: rejects (interface face,
// same capture-free gate).
// T6 — static method reference on ISharedEvent<int>.Subscribe: ACCEPT
// end-to-end (codegen's ISharedEvent.Subscribe arm claims a static ref,
// and #4901 v3's signature gate `sharedEventHandlerAdmissible` lets the
// `(int) -> ()` shape through).
//
// REJECT rows assert a diagnostic that names either §6.5 (capture-freedom,
// #4920) or §6.5/§27.1.1 (handler slot, #4898) — both are sufficient;
// pre-fix, NEITHER fired (the slot check was missing AND #4920 hadn't
// landed).

#include <string>

#include "support/aot_support.hpp"  // runAot — AOT + native exec oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool hasDiag(const bezel::test::JitResult& r, const char* a,
             const char* b = nullptr) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(a) != std::string::npos &&
            (!b || d.message.find(b) != std::string::npos))
            return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// REJECT — bound method on SharedEventSource.Subscribe (capture-freedom gate
// #4920 fires first; the slot check #4898 also rejects).
// ---------------------------------------------------------------------------
BEZEL_TEST(shared_event_source_handler_signature_4898,
           shared_eventsource_subscribe_bound_method_rejected_spec_6_5_27_1_1) {
    auto r = runAot(
        "module M; "
        "class H "
        "  public method Handle(n: int) Console.WriteLine($\"v:{n}\"); end Handle; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    using var src := new SharedEventSource<int>(); "
        "    var h := new H(); "
        "    src.Subscribe(h.Handle); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    // #4920's §6.5 capture-freedom diagnostic names "capture-free".
    BEZEL_EXPECT_TRUE(hasDiag(r, "handler subscribed to an ISharedEvent<T> or "
                                  "SharedEventSource<T> must be capture-free"));
}

// ---------------------------------------------------------------------------
// REJECT — capturing lambda on SharedEventSource.Subscribe (§6.5
// capture-freedom admission, #4920).
// ---------------------------------------------------------------------------
BEZEL_TEST(shared_event_source_handler_signature_4898,
           shared_eventsource_subscribe_capturing_lambda_rejected_spec_6_5_28_14) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var captured: int := 7; "
        "    using var src := new SharedEventSource<int>(); "
        "    var h := lambda(x: int) => do Console.WriteLine(captured + x); end; "
        "    src.Subscribe(h); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "handler subscribed to an ISharedEvent<T> or "
                                  "SharedEventSource<T> must be capture-free"));
}

// ---------------------------------------------------------------------------
// ACCEPT — static method reference on SharedEventSource.Subscribe runs and
// prints the payload (codegen's `sharedEventHandlerAdmissible` claims the
// static method ref, v3's `(T) -> ()` shape gate lets it through).
// ---------------------------------------------------------------------------
BEZEL_TEST(shared_event_source_handler_signature_4898,
           shared_eventsource_subscribe_static_method_runs_spec_6_5) {
    auto r = runAot(
        "module M; "
        "class H "
        "  public static method On(n: int) Console.WriteLine($\"v:{n}\"); end On; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    using var src := new SharedEventSource<int>(); "
        "    src.Subscribe(H.On); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// fix #4972 (stdlib §6.5 v0.307, spec §28.14 v0.307): capture-free lambda
// on SharedEventSource.Subscribe. Was a CONTROL pinning the codegen gap
// (the handler slot check #4898 + capture-freedom admission #4920 passed,
// then codegen rejected with #2457); #4972 lifts the codegen gap and the
// handler now runs, printing its payload.
// ---------------------------------------------------------------------------
BEZEL_TEST(shared_event_source_handler_signature_4898,
           shared_eventsource_subscribe_capture_free_lambda_control_spec_6_5) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var src := new SharedEventSource<int>(); "
        "    src.Subscribe(lambda(x: int) => Console.WriteLine($\"v:{x}\")); "
        "    src.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C;");
    // FLIPPED by #4972: was the CONTROL #2457 reject row on b640eb0d/l24b
    // (codegen rejected with "produced no value, and no dispatch arm
    // emitted any code"); now asserts the spec-stated behaviour — the
    // lambda handler runs, prints "v:9", and exits 0.
    BEZEL_EXPECT_TRUE(r.ran && r.exitCode == 0);
    BEZEL_EXPECT_TRUE(r.stdoutText == "v:9\n" || r.stdoutText == "v:9\r\n");
}

// ---------------------------------------------------------------------------
// REJECT — bound method on ISharedEvent<int>.Subscribe (interface face;
// #4920's capture-free gate fires first).
// ---------------------------------------------------------------------------
BEZEL_TEST(shared_event_source_handler_signature_4898,
           isharedevent_subscribe_bound_method_rejected_spec_6_5) {
    auto r = runAot(
        "module M; "
        "class Bus "
        "  private static owned readonly _feed: SharedEventSource<int> := new(); "
        "  public static property Feed: ISharedEvent<int> => _feed; "
        "end Bus; "
        "class H "
        "  public method Handle(n: int) Console.WriteLine($\"v:{n}\"); end Handle; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var h := new H(); "
        "    Bus.Feed.Subscribe(h.Handle); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "handler subscribed to an ISharedEvent<T> or "
                                  "SharedEventSource<T> must be capture-free"));
}

// ---------------------------------------------------------------------------
// ACCEPT — static method reference on ISharedEvent<int>.Subscribe runs
// (codegen's ISharedEvent.Subscribe arm claims the static ref).
// ---------------------------------------------------------------------------
BEZEL_TEST(shared_event_source_handler_signature_4898,
           isharedevent_subscribe_static_method_runs_spec_6_5) {
    auto r = runAot(
        "module M; "
        "class Bus "
        "  private static owned readonly _feed: SharedEventSource<int> := new(); "
        "  public static property Feed: ISharedEvent<int> => _feed; "
        "end Bus; "
        "class H "
        "  public static method On(n: int) Console.WriteLine($\"v:{n}\"); end On; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    Bus.Feed.Subscribe(H.On); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// FAIL-WITHOUT PIN ROWS — these pin the §6.5 / §27.1.1 HANDLER-SLOT check
// itself (not the §6.5 capture-free admission). Each uses a STATIC method
// reference (capture-free per #4920) whose signature does NOT match the
// stdlib-stated `(T) -> ()` handler slot — pre-fix the handler slot was
// missing entirely (plainEventHandlerSlot excluded the SharedEventSource/
// ISharedEvent pair), so each row SILENTLY ACCEPTED the wrong-typed
// handler and codegen emitted it against the raw payload. Post-fix the
// same rows reject with the §27.1.1 / §27.6 handler-argument diagnostic.
// ===========================================================================

// F1 — arity-2 handler on SharedEventSource<int>.Subscribe: pre-fix the
// extra argument was silently ignored and codegen invoked a handler
// reading past the payload (measured against the plain-event #4968
// sibling row c08: garbage `5/<junk>`). Post-fix the arity reject fires.
BEZEL_TEST(shared_event_source_handler_signature_4898,
           fail_without_shared_eventsource_subscribe_arity_two_rejected_spec_27_1_1) {
    auto r = runAot(
        "module M; "
        "class H "
        "  public static method On(x: int, y: int) Console.WriteLine($\"{x}/{y}\"); end On; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    using var src := new SharedEventSource<int>(); "
        "    src.Subscribe(H.On); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    // The §27.1.1 arity reject is emitted by `checkEventHandlerArgument`
    // when the handler is a LambdaExpr ("event handler lambda has N
        // parameters, but `Subscribe` on an event source is
        // `Subscribe(handler)`"); for a method reference, the
        // shape mismatch surfaces via the same helper's `checkAssignable`
        // (§27.1.1 first bullet: arity mismatch is never assignable).
    BEZEL_EXPECT_TRUE(hasDiag(r, "not implicitly convertible"));
}

// F2 — `owned` formal on SharedEventSource<int>.Add: pre-fix the owned
// parameter compiled, the handler ran against the payload, and the
// payload was freed mid-raise (the plain-event #4968 sibling c09 was a
// heap-use-after-free on the second subscriber). Post-fix the §27.1
// modifier-slot reject fires (modifier slots are part of a function
// signature's identity).
BEZEL_TEST(shared_event_source_handler_signature_4898,
           fail_without_shared_eventsource_add_owned_formal_rejected_spec_27_1) {
    auto r = runAot(
        "module M; "
        "class Msg "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end; "
        "end Msg; "
        "class H "
        "  public static method OnB(owned m: Msg) Console.WriteLine($\"B:{m.N}\"); end OnB; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    using var src := new SharedEventSource<Msg>(); "
        "    src.Add(H.OnB); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "owned"));
}

// F3 — value-returning handler on SharedEventSource<int>.Subscribe:
// pre-fix the handler ran and its return value was discarded (the
// silent wrong-code the plain-event #4968 sibling c13 was a deliberate
// spurious-reject note for sync events; the §6.5 sync handler type IS
// `(T) -> ()` and §27.1 makes the return part of the function type).
// Post-fix the return-slot reject fires for a formed method reference.
BEZEL_TEST(shared_event_source_handler_signature_4898,
           fail_without_shared_eventsource_subscribe_value_returning_handler_rejected_spec_27_1) {
    auto r = runAot(
        "module M; "
        "class H "
        "  public static method OnI(x: int): int Console.WriteLine($\"r:{x}\"); return x; end OnI; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    using var src := new SharedEventSource<int>(); "
        "    src.Subscribe(H.OnI); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasDiag(r, "handler argument of"));
}