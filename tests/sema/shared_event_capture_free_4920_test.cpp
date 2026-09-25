// Spec-derived regression tests for issue #4920 — stdlib §6.5 (v0.307) / spec
// §28.14 (v0.307) capture-freedom admission at every Subscribe / Add / +=
// position on the ISharedEvent<T> / SharedEventSource<T> cross-domain
// publication-point pair.
//
// The §6.5 / §28.14 admission is: every Subscribe position of the pair
// (`Subscribe` on the interface and the class, `Add`, and the `+=`
// desugaring, spec §17.2) admits ONLY a capture-free handler — a
// non-capturing lambda or a static method reference. The check is keyed on
// the resolved callee (the stdlib-stated §28.14 register, mirroring
// `Domain.Spawn`'s own admission; #4920 generalizes the existing static-
// event capture-freedom check at ExprTypecheck_Exprs.cpp:9237 /
// ExprTypecheck.cpp:18148 to the ISharedEvent<T> / SharedEventSource<T>
// pair, which has no EventSymbol and no member-keyword `static event`).
//
// Why this is TIER-1: on f321f94e (this lane's base), an ISharedEvent<T> /
// SharedEventSource<T> Subscribe / Add / += has NO codegen claim at all —
// the #4901 codegen narrow (claim only STATIC method-ref handlers,
// fall everything else to the #2457 "not yet implemented" backstop) is
// NOT yet in this base, so EVERY handler shape on this pair fails at
// codegen with #2457. That accidental backstop is load-bearing for
// correctness today, but it is fragile — once L24b widens the codegen
// arm to claim a capturing handler (the natural next step for #4901's
// rework), capturing handlers would compile AND produce runtime
// heap-use-after-free (the five probes rv-l24/p/{p3b_inst_add,
// p8_add_local_escape, p10_this_method, p11_face_inst, p5_cond} all
// measured a heap-UAF under generated-code ASan when codegen claimed a
// capturing handler). This lane puts the gate at SEMa so a future
// codegen widening is contained — and so the test rows here pin the
// soundness property rather than the codegen narrow.
//
// Test shape (per main's oracle clarification + structural correction):
//
//   FAIL-WITHOUT pins (the bug pins — these rows MUST differ
//   pre-fix-vs-post-fix):
//     p1_local, p3_inst, p3b_inst_add, p5_cond, p7_face_local,
//     p8_add_local_escape, p10_this_method x2, p11_face_inst, pe_capture.
//
//   CAPTURE-FREE-LAMBDA rows (CONTROL on f321f94e — sema now admits them
//   via #4920, but codegen still hits the #2457 backstop because #4901's
//   narrow is not yet on this base; they flip to a RUN-with-stdout row
//   when L24b widens the codegen claim):
//     p9_static_ctl (static method ref, NOT a lambda), pe_free, pe_free_via_add.
//     Marked CONTROL — identical outcome on pre/post (#2457 in both).
//
//   STATIC-METHOD-REF rows (also CONTROL on f321f94e — same reason as the
//   capture-free-lambda rows: codegen still rejects):
//     p9_static_ctl pe_static.
//
// The new §6.5 diagnostic text (stdlib §6.5, v0.307 / spec §28.14, v0.307)
// is asserted on every FAIL-WITHOUT pin, with `diagnosticAbsent(#2457)`
// guarding the diagnostic-text shift (pre-fix the row carries #2457, not
// §6.5; post-fix the row carries §6.5, not #2457). The CAPTURE-FREE rows
// assert the EXACT #2457 needle — pre-fix and post-fix both — to pin the
// intermediate state (sema admits, codegen rejects).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Returns true iff `r` is a compile-time rejection (NOT a runtime crash /
// signal / timeout / ASan report) whose diagnostic carries `needle` as a
// substring. This is the strict-positive form: a row that asserts
// "the compiler rejected this with §6.5" cannot pass on a crash/timeout
// (cf. tests/support/aot_result_asserts.hpp file header).
inline bool rejectedWithNeedle(const bezel::test::JitResult& r,
                               const char* needle) {
    return failedAtCompileTimeWithRealLocation(r) &&
           diagnosticMessageContains(r, needle);
}

// Returns true iff NO diagnostic in `r` contains `needle`. Used to assert
// that the §6.5 capture diagnostic fires INSTEAD of the #2457 codegen
// backstop on the FAIL-WITHOUT rows — pre-fix these rows would carry
// #2457, post-fix they must carry §6.5 only.
inline bool diagnosticAbsent(const bezel::test::JitResult& r,
                            const char* needle) {
    return !diagnosticMessageContains(r, needle);
}

}  // namespace

// ===========================================================================
// FAIL-WITHOUT pins — capturing handlers on the ISharedEvent<T> /
// SharedEventSource<T> Subscribe / Add / += positions.
//
// On f321f94e, these programs reject at codegen with #2457 (no dispatch
// arm emitted any code); post-#4920, sema rejects with the §6.5 / §28.14
// capture-freedom diagnostic. The diagnostic-text shift is the FAIL-WITHOUT
// evidence.
// ===========================================================================

// p1_local: capturing lambda held in a local, passed to Subscribe. The
// receiver is SharedEventSource<T>; the handler is a closure carrying
// `box`. Pre-fix: codegen #2457. Post-fix: sema §6.5.
BEZEL_TEST(sema_shared_event_capture_free_4920,
           subscribe_capturing_lambda_local_rejected_spec_6_5_28_14) {
    auto r = runAot(
        "module M; "
        "import System.Threading; "
        "class Bus4920p1 "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "end Bus4920p1; "
        "class Box4920p1 "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box4920p1; "
        "class C4920p1 "
        "  public static method Main(): int "
        "    var box := new Box4920p1(); "
        "    var h := lambda(x: int) => do box.Add(x); end; "
        "    var sub := Bus4920p1.Feed.Subscribe(h); "
        "    Bus4920p1.Feed.Raise(9); "
        "    sub.Dispose(); "
        "    Console.WriteLine(box.V); "
        "    return 0; "
        "  end Main; "
        "end C4920p1;");
    BEZEL_EXPECT_TRUE(rejectedWithNeedle(r,
        "handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must be capture-free"));
    BEZEL_EXPECT_TRUE(diagnosticAbsent(r,
        "produced no value, and no dispatch arm emitted any code"));
}

// p3_inst: instance method group `box.OnI` on Subscribe. The receiver is
// SharedEventSource<T>; the handler is a bound-instance method reference
// (captures `box`).
BEZEL_TEST(sema_shared_event_capture_free_4920,
           subscribe_instance_method_group_rejected_spec_6_5_28_14) {
    auto r = runAot(
        "module M; "
        "import System.Threading; "
        "class Bus4920p3 "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "end Bus4920p3; "
        "class Box4920p3 "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "  public method OnI(x: int) V := V + x; end OnI; "
        "end Box4920p3; "
        "class C4920p3 "
        "  public static method Main(): int "
        "    var box := new Box4920p3(); "
        "    var sub := Bus4920p3.Feed.Subscribe(box.OnI); "
        "    Bus4920p3.Feed.Raise(9); "
        "    sub.Dispose(); "
        "    Console.WriteLine(box.V); "
        "    return 0; "
        "  end Main; "
        "end C4920p3;");
    BEZEL_EXPECT_TRUE(rejectedWithNeedle(r,
        "handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must be capture-free"));
    BEZEL_EXPECT_TRUE(diagnosticAbsent(r,
        "produced no value, and no dispatch arm emitted any code"));
}

// p3b_inst_add: Add(box.OnI) in a helper. Captures via instance method
// group on the Add position.
BEZEL_TEST(sema_shared_event_capture_free_4920,
           add_instance_method_group_rejected_spec_6_5_28_14) {
    auto r = runAot(
        "module M; "
        "import System.Threading; "
        "class Bus4920p3b "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "end Bus4920p3b; "
        "class Box4920p3b "
        "  public V: int := 0; "
        "  public method OnI(x: int) V := V + x; end OnI; "
        "end Box4920p3b; "
        "class C4920p3b "
        "  public static method Arm() "
        "    var box := new Box4920p3b(); "
        "    Bus4920p3b.Feed.Add(box.OnI); "
        "  end Arm; "
        "  public static method Main(): int "
        "    Arm(); "
        "    Bus4920p3b.Feed.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C4920p3b;");
    BEZEL_EXPECT_TRUE(rejectedWithNeedle(r,
        "handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must be capture-free"));
    BEZEL_EXPECT_TRUE(diagnosticAbsent(r,
        "produced no value, and no dispatch arm emitted any code"));
}

// p5_cond: Subscribe((if c then f else Bus.OnX)) where f is a capturing
// lambda local. Conditional over capturing + non-capturing arms — the
// capturing arm is enough to reject the whole expression (the runtime
// shape of the conditional matters; stdlib §6.5 the check is keyed on
// the resolved callee, which is Subscribe — the handler is a single
// expression that could at runtime be the capturing arm).
BEZEL_TEST(sema_shared_event_capture_free_4920,
           subscribe_conditional_over_capturing_rejected_spec_6_5_28_14) {
    auto r = runAot(
        "module M; "
        "import System.Threading; "
        "class Bus4920p5 "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "  public static Total: int := 0; "
        "  public static method OnX(x: int) Total := Total + x; end OnX; "
        "end Bus4920p5; "
        "class Box4920p5 "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box4920p5; "
        "class C4920p5 "
        "  public static method Main(): int "
        "    var box := new Box4920p5(); "
        "    var c := true; "
        "    var f := lambda(x: int) => do box.Add(x); end; "
        "    var sub := Bus4920p5.Feed.Subscribe((if c then f else Bus4920p5.OnX)); "
        "    Bus4920p5.Feed.Raise(9); "
        "    sub.Dispose(); "
        "    Console.WriteLine(box.V); "
        "    return 0; "
        "  end Main; "
        "end C4920p5;");
    BEZEL_EXPECT_TRUE(rejectedWithNeedle(r,
        "handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must be capture-free"));
    BEZEL_EXPECT_TRUE(diagnosticAbsent(r,
        "produced no value, and no dispatch arm emitted any code"));
}

// p7_face_local: through ISharedEvent<T> face. The receiver is the
// interface face (declared as `ISharedEvent<int>`, the subscribe-only
// interface, per stdlib §6.5 — distinct from SharedEventSource<T>); the
// handler is a capturing lambda local. Detection must key on the
// receiver TYPE (ISharedEvent), not on the runtime concrete type.
BEZEL_TEST(sema_shared_event_capture_free_4920,
           subscribe_face_capturing_lambda_local_rejected_spec_6_5_28_14) {
    auto r = runAot(
        "module M; "
        "import System.Threading; "
        "class Bus4920p7 "
        "  private static owned readonly _feed: SharedEventSource<int> := new(); "
        "  public static property Feed: ISharedEvent<int> => _feed; "
        "  public static method Publish(x: int) _feed.Raise(x); end Publish; "
        "end Bus4920p7; "
        "class Box4920p7 "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box4920p7; "
        "class C4920p7 "
        "  public static method Main(): int "
        "    var box := new Box4920p7(); "
        "    var h := lambda(x: int) => do box.Add(x); end; "
        "    var sub := Bus4920p7.Feed.Subscribe(h); "
        "    Bus4920p7.Publish(4); "
        "    sub.Dispose(); "
        "    Console.WriteLine(box.V); "
        "    return 0; "
        "  end Main; "
        "end C4920p7;");
    BEZEL_EXPECT_TRUE(rejectedWithNeedle(r,
        "handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must be capture-free"));
    BEZEL_EXPECT_TRUE(diagnosticAbsent(r,
        "produced no value, and no dispatch arm emitted any code"));
}

// p8_add_local_escape: capturing lambda local passed to Add in a helper.
// Add position on SharedEventSource<T>.
BEZEL_TEST(sema_shared_event_capture_free_4920,
           add_capturing_lambda_local_rejected_spec_6_5_28_14) {
    auto r = runAot(
        "module M; "
        "import System.Threading; "
        "class Bus4920p8 "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "end Bus4920p8; "
        "class Box4920p8 "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box4920p8; "
        "class C4920p8 "
        "  public static method Arm() "
        "    var box := new Box4920p8(); "
        "    var h := lambda(x: int) => do box.Add(x); Console.WriteLine(box.V); end; "
        "    Bus4920p8.Feed.Add(h); "
        "  end Arm; "
        "  public static method Main(): int "
        "    Arm(); "
        "    Bus4920p8.Feed.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C4920p8;");
    BEZEL_EXPECT_TRUE(rejectedWithNeedle(r,
        "handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must be capture-free"));
    BEZEL_EXPECT_TRUE(diagnosticAbsent(r,
        "produced no value, and no dispatch arm emitted any code"));
}

// p10_this_method (`OnI` form): inside an instance method, `Feed.Add(OnI)`.
// `OnI` resolves as an unqualified IdentifierExpr; per §30.14 it captures
// `this` (the receiver of the enclosing method) — a non-static, bound-
// instance-method reference. Post-#4920 this is rejected by the §6.5
// capture-freedom admission on SharedEventSource<T>.Add.
BEZEL_TEST(sema_shared_event_capture_free_4920,
           add_unqualified_this_method_rejected_spec_6_5_28_14) {
    auto r = runAot(
        "module M; "
        "import System.Threading; "
        "class Bus4920p10 "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "end Bus4920p10; "
        "class Box4920p10 "
        "  public V: int := 0; "
        "  public method OnI(x: int) V := V + x; Console.WriteLine(V); end OnI; "
        "  public method Arm() Bus4920p10.Feed.Add(OnI); end Arm; "
        "end Box4920p10; "
        "class C4920p10 "
        "  public static method Go() "
        "    var b := new Box4920p10(); "
        "    b.Arm(); "
        "  end Go; "
        "  public static method Main(): int "
        "    Go(); "
        "    Bus4920p10.Feed.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C4920p10;");
    BEZEL_EXPECT_TRUE(rejectedWithNeedle(r,
        "handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must be capture-free"));
    BEZEL_EXPECT_TRUE(diagnosticAbsent(r,
        "produced no value, and no dispatch arm emitted any code"));
}

// p10_this_method (`this.OnI` form): same defect, qualified spelling.
BEZEL_TEST(sema_shared_event_capture_free_4920,
           add_qualified_this_method_rejected_spec_6_5_28_14) {
    auto r = runAot(
        "module M; "
        "import System.Threading; "
        "class Bus4920p10b "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "end Bus4920p10b; "
        "class Box4920p10b "
        "  public V: int := 0; "
        "  public method OnI(x: int) V := V + x; Console.WriteLine(V); end OnI; "
        "  public method Arm() Bus4920p10b.Feed.Add(this.OnI); end Arm; "
        "end Box4920p10b; "
        "class C4920p10b "
        "  public static method Go() "
        "    var b := new Box4920p10b(); "
        "    b.Arm(); "
        "  end Go; "
        "  public static method Main(): int "
        "    Go(); "
        "    Bus4920p10b.Feed.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C4920p10b;");
    BEZEL_EXPECT_TRUE(rejectedWithNeedle(r,
        "handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must be capture-free"));
    BEZEL_EXPECT_TRUE(diagnosticAbsent(r,
        "produced no value, and no dispatch arm emitted any code"));
}

// p11_face_inst: through ISharedEvent<T> face with an instance method
// group. Detection is on the receiver's static type (ISharedEvent<int>).
BEZEL_TEST(sema_shared_event_capture_free_4920,
           subscribe_face_instance_method_group_rejected_spec_6_5_28_14) {
    auto r = runAot(
        "module M; "
        "import System.Threading; "
        "class Bus4920p11 "
        "  private static owned readonly _feed: SharedEventSource<int> := new(); "
        "  public static property Feed: ISharedEvent<int> => _feed; "
        "  public static method Publish(x: int) _feed.Raise(x); end Publish; "
        "end Bus4920p11; "
        "class Box4920p11 "
        "  public V: int := 0; "
        "  public method OnI(x: int) V := V + x; Console.WriteLine(V); end OnI; "
        "end Box4920p11; "
        "class C4920p11 "
        "  public static method Go() "
        "    var b := new Box4920p11(); "
        "    var sub := Bus4920p11.Feed.Subscribe(b.OnI); "
        "  end Go; "
        "  public static method Main(): int "
        "    Go(); "
        "    Bus4920p11.Publish(3); "
        "    return 0; "
        "  end Main; "
        "end C4920p11;");
    BEZEL_EXPECT_TRUE(rejectedWithNeedle(r,
        "handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must be capture-free"));
    BEZEL_EXPECT_TRUE(diagnosticAbsent(r,
        "produced no value, and no dispatch arm emitted any code"));
}

// pe_capture: `Bus.Feed += lambda(x) box.Add(x);` with a capturing lambda
// on the += position (desugars to .Add per spec §17.2). The diagnostic
// text is the SharedEventSource-via-`+=` form, distinct from the
// Subscribe/Add form above.
BEZEL_TEST(sema_shared_event_capture_free_4920,
           plus_equal_capturing_lambda_rejected_spec_6_5_17_2_28_14) {
    auto r = runAot(
        "module M; "
        "import System.Threading; "
        "class Bus4920pec "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "end Bus4920pec; "
        "class Box4920pec "
        "  public V: int := 0; "
        "  public method Add(x: int) V := V + x; end Add; "
        "end Box4920pec; "
        "class C4920pec "
        "  public static method Main(): int "
        "    var box := new Box4920pec(); "
        "    Bus4920pec.Feed += lambda(x: int) => do box.Add(x); end; "
        "    Bus4920pec.Feed.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C4920pec;");
    BEZEL_EXPECT_TRUE(rejectedWithNeedle(r,
        "handler subscribed to a SharedEventSource<T> via `+=` must be capture-free"));
    BEZEL_EXPECT_TRUE(diagnosticAbsent(r,
        "produced no value, and no dispatch arm emitted any code"));
}

// ===========================================================================
// CONTROL rows — capture-free handlers.
//
// Intermediate state on f321f94e: sema now admits a capture-free handler
// (#4920's check passes), but codegen has no claim for any handler shape
// on the ISharedEvent<T> / SharedEventSource<T> pair (#4901's codegen
// narrow is not in this base), so codegen rejects with the #2457
// backstop. These rows are CONTROLS — identical outcome (compile fails
// with #2457) on pre- and post-#4920. They flip to a RUN-with-stdout
// row when L24b's codegen claim lands (per the brief's intermediate-state
// clarification + the structural correction on FAIL-WITHOUT).
// ===========================================================================

// p9_static_ctl (the spec's static-control example): Subscribe(Bus.OnX).
// Static method ref — isCaptureFreeEntry returns true; sema admits. The
// diagnostic-text shift is absent on this row (pre-fix and post-fix both
// carry #2457 because codegen has no claim for this pair on f321f94e).
// When L24b's codegen narrow lands, the row flips to RUN-with-stdout="9"
// — the comment is the canonical reference for that future assertion.
BEZEL_TEST(sema_shared_event_capture_free_4920,
           subscribe_static_method_ref_control_spec_6_5_28_14) {
    auto r = runAot(
        "module M; "
        "import System.Threading; "
        "class Bus4920p9 "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "  public static Total: int := 0; "
        "  public static method OnX(x: int) Total := Total + x; end OnX; "
        "end Bus4920p9; "
        "class C4920p9 "
        "  public static method Main(): int "
        "    var sub := Bus4920p9.Feed.Subscribe(Bus4920p9.OnX); "
        "    Bus4920p9.Feed.Raise(9); "
        "    sub.Dispose(); "
        "    Console.WriteLine(Bus4920p9.Total); "
        "    return 0; "
        "  end Main; "
        "end C4920p9;");
    // CONTROL: sema admits (capture-free), codegen rejects with #2457.
    // Intermediate state — flips to RUN-with-stdout="9" when L24b widens
    // the codegen claim for SharedEventSource<T>.Subscribe.
    BEZEL_EXPECT_TRUE(rejectedWithNeedle(r,
        "produced no value, and no dispatch arm emitted any code"));
}

// pe_free: capture-free lambda literal on Subscribe. Sema admits
// (capture-free); codegen rejects with #2457 (no claim).
BEZEL_TEST(sema_shared_event_capture_free_4920,
           subscribe_capture_free_lambda_control_spec_6_5_28_14) {
    auto r = runAot(
        "module M; "
        "import System.Threading; "
        "class Bus4920pef "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "end Bus4920pef; "
        "class C4920pef "
        "  public static method Main(): int "
        "    var sub := Bus4920pef.Feed.Subscribe(lambda(x: int) => Console.WriteLine(x)); "
        "    Bus4920pef.Feed.Raise(9); "
        "    sub.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C4920pef;");
    // CONTROL: same intermediate state as p9_static_ctl above.
    BEZEL_EXPECT_TRUE(rejectedWithNeedle(r,
        "produced no value, and no dispatch arm emitted any code"));
}

// pe_free_via_add: capture-free lambda held in a local, passed to Add.
// NB: `isCaptureFreeEntry` (ExprTypecheck_Calls.cpp:26227) does NOT
// dereference a LOCAL that holds a lambda — its IdentifierExpr arm only
// recognizes a static MethodSymbol, returning false for a LocalSymbol.
// So a local holding a (capture-free) lambda is rejected at sema with
// the §6.5 capture diagnostic, exactly like a local holding a
// CAPTURING lambda. This is intentional per spec §28.14 "forming a
// method reference over a capture-free-register position is a compile
// error" (the local indirection erases the admission; the brief's
// guidance "DO NOT modify isCaptureFreeEntry" implies this is the
// accepted state). FAIL-WITHOUT pin: pre-fix #2457, post-fix §6.5.
BEZEL_TEST(sema_shared_event_capture_free_4920,
           add_capture_free_lambda_local_rejected_spec_6_5_28_14) {
    auto r = runAot(
        "module M; "
        "import System.Threading; "
        "class Bus4920pefl "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "end Bus4920pefl; "
        "class C4920pefl "
        "  public static method Main(): int "
        "    var h := lambda(x: int) => Console.WriteLine(x); "
        "    Bus4920pefl.Feed.Add(h); "
        "    Bus4920pefl.Feed.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C4920pefl;");
    // FAIL-WITHOUT pin — diagnostic-text shift from #2457 (pre-fix) to
    // §6.5 (post-fix). The capture-free LAMBDA inside `h` is opaque to
    // sema because the predicate doesn't introspect locals; the form is
    // rejected as if `h` were capturing, per the §28.14 admission-erase
    // rule noted above.
    BEZEL_EXPECT_TRUE(rejectedWithNeedle(r,
        "handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must be capture-free"));
    BEZEL_EXPECT_TRUE(diagnosticAbsent(r,
        "produced no value, and no dispatch arm emitted any code"));
}

// pe_static: static method ref via +=. Desugars to .Add per spec §17.2.
// Sema admits (capture-free); codegen rejects with #2457 (no claim).
// CONTROL — flips to RUN-with-stdout when L24b widens.
BEZEL_TEST(sema_shared_event_capture_free_4920,
           plus_equal_static_method_ref_control_spec_6_5_17_2_28_14) {
    auto r = runAot(
        "module M; "
        "import System.Threading; "
        "class Bus4920pes "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "  public static Total: int := 0; "
        "  public static method OnX(x: int) Total := Total + x; end OnX; "
        "end Bus4920pes; "
        "class C4920pes "
        "  public static method Main(): int "
        "    Bus4920pes.Feed += Bus4920pes.OnX; "
        "    Bus4920pes.Feed.Raise(9); "
        "    Console.WriteLine(Bus4920pes.Total); "
        "    return 0; "
        "  end Main; "
        "end C4920pes;");
    // CONTROL: same intermediate state as p9_static_ctl above.
    BEZEL_EXPECT_TRUE(rejectedWithNeedle(r,
        "produced no value, and no dispatch arm emitted any code"));
}

// pe_free_pe: capture-free lambda literal via +=. Desugars to .Add per
// spec §17.2. Sema admits (capture-free); codegen rejects with #2457.
// CONTROL — flips to RUN-with-stdout when L24b widens.
BEZEL_TEST(sema_shared_event_capture_free_4920,
           plus_equal_capture_free_lambda_control_spec_6_5_17_2_28_14) {
    auto r = runAot(
        "module M; "
        "import System.Threading; "
        "class Bus4920pefp "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "end Bus4920pefp; "
        "class C4920pefp "
        "  public static method Main(): int "
        "    Bus4920pefp.Feed += lambda(x: int) => Console.WriteLine(x); "
        "    Bus4920pefp.Feed.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C4920pefp;");
    // CONTROL: same intermediate state as p9_static_ctl above.
    BEZEL_EXPECT_TRUE(rejectedWithNeedle(r,
        "produced no value, and no dispatch arm emitted any code"));
}