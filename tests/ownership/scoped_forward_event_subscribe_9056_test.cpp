// Ownership / codegen over-release — fix #9056. A class forwards its `scoped`
// handler parameter to `EventSource<T>.Subscribe(scoped handler)`; the program
// compiled, then aborted at teardown with `refcount over-release (closure)`
// under checked disposal (a heap-use-after-free at the first `Raise` under
// ASan, a segfault in a plain build).
//
// Spec: §28.14 (scoped callback parameters — the callee may invoke the
// callback, store it in a `scoped` field of the bounding object, "or forward
// it to another `scoped` parameter"), §28.9 (runtime-managed types — the
// closure object is released exactly once), §28.3 (a by-value function-typed
// parameter is a BORROW), stdlib §6.5 (`EventSource<T>.Subscribe` /
// `.Add(scoped handler)` store the handler until the `Subscription` is
// disposed or the source goes away) and stdlib §6.1
// (`CancellationToken.Register(scoped callback)`). Spec revision v0.359.
//
// THE DEFECT. Since #8817 the caller of a method releases a `+1` producer (a
// lambda literal or a method reference) passed at a `scoped` formal once the
// call returns, on the argument that every store the callee makes takes a
// reference of its own (the `scoped` field store retains). The storing stdlib
// sinks did not: `bz_event_subscribe` / `bz_event_add` / `bz_ctoken_register`
// take the reference they are handed as a TRANSFERRED +1 (#841) and release it
// when the registration ends. A forwarded `scoped` formal is a +0 borrow, so
// the closure was released twice — by the caller after the call, and by the
// subscription — and the sink invoked it after the first release.
//
// THE FIX. The sink takes its own reference when its handler is a bare read of
// a `scoped` formal of the method being emitted (or of the expression-bodied
// generic method inlined at the call), gated on the same callee-shape
// predicate as the caller's release (`scopedFormalCallerReleasesProducer`).
// The caller's closure LOCAL passed at such a formal now keeps its +1 and
// releases it at its own scope end (it was treated as an escape and never
// released — rows H and J), so the two spellings of the caller agree. The
// same discipline is applied to an IMPORTED callee (see
// tests/module/xmod_scoped_forward_event_subscribe_9056_test.cpp).
//
// (Rows F and K were FLIPPED to rejections by fix #9201: their arrow-bodied
// forwards resolve no `this` bound under §28.14's forms; see each row.)
//
// ROWS A–K are RED on r30-c1 (MEASURED with --no-results-cache: A–G, I and K
// abort or report a use-after-free; H and J leak the closure each iteration)
// and GREEN with the fix. Two BOUNDARY CONTROLS (base-green, named in the
// commit body): CTRL1, a closure local forwarded to the same sink, which a
// sink retain WITHOUT the caller-local relief turns into a leak; and CTRL2,
// the direct `src.Subscribe(lambda …)`, which a retain that ignored the
// "forwarded formal" condition turns into a leak.
//
// Every row loops 200x and prints the per-iteration hit count, so a missed or
// doubled invocation shows in stdout and a per-iteration leak cannot hide.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// `n` lines each reading `v`.
std::string repeatLine(const std::string& v, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += v + "\n";
    return out;
}

const char* const kCounter =
    "module P; "
    "public class Counter2 "
    "  private _n: int := 0; "
    "  public method Hit(t: double) => _n := _n + 1; "
    "  public property N: int => _n; "
    "end Counter2; ";

const char* const kHub =
    "public class Hub "
    "  private owned readonly _changed: EventSource<double> := "
    "    new EventSource<double>(); "
    "  public method OnChanged(scoped handler: (double) -> ()): owned Subscription "
    "    => _changed.Subscribe(handler); "
    "  public method On(scoped handler: (double) -> ()) => _changed.Add(handler); "
    "  public method Fire(t: double) => _changed.Raise(t); "
    "end Hub; ";

// A 200-iteration Main whose body is `body`, printing `c.N` per iteration.
std::string program(const std::string& types, const std::string& body) {
    return std::string(kCounter) + types +
           "public class Program "
           "  public static method Main() "
           "    for var i in 0..<200 do "
           "      using var c := new Counter2(); " +
           body +
           "      Console.WriteLine(c.N); "
           "    end; "
           "  end Main; "
           "end Program;";
}

void expectClean(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// §28.14's forward diagnostic for the formal `name`, verbatim (fix #9201: the
// arrow body now runs the callee-side walk its block twin runs).
std::string scopedForward(const std::string& name) {
    return "`" + name +
           "` is a `scoped` parameter — it may be forwarded only to another "
           "`scoped` parameter of a call on the same bounding object (`this`) "
           "(§28.14, v0.175)";
}

void expectForwardRejected(const JitResult& r, const std::string& name) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, scopedForward(name).c_str()));
}

}  // namespace

// A — THE ISSUE'S OWN REPRO: a capturing lambda literal forwarded through a
// `scoped` formal to `EventSource<double>.Subscribe`, Raised once. Base: a
// heap-use-after-free at the Raise / `refcount over-release (closure)`.
BEZEL_TEST(ownership_9056_scoped_forward_sink,
           forwarded_lambda_to_subscribe_spec_28_14_issue_repro) {
    auto r = runAotChecked(program(kHub,
        "      using var hub := new Hub(); "
        "      using var sub := hub.OnChanged(lambda(t) => c.Hit(t)); "
        "      hub.Fire(1.0); "));
    expectClean(r, repeatLine("1", 200));
}

// B — the forwarded capturing lambda is Raised SEVERAL times: every Raise
// must invoke the live closure exactly once (3 per iteration).
BEZEL_TEST(ownership_9056_scoped_forward_sink,
           forwarded_lambda_raised_three_times_spec_28_14) {
    auto r = runAotChecked(program(kHub,
        "      using var hub := new Hub(); "
        "      using var sub := hub.OnChanged(lambda(t) => c.Hit(t)); "
        "      hub.Fire(1.0); hub.Fire(2.0); hub.Fire(3.0); "));
    expectClean(r, repeatLine("3", 200));
}

// C — a forwarded METHOD GROUP (`c.Hit`, a +1 method-reference producer at the
// caller, §27.5) through the same `scoped` formal.
BEZEL_TEST(ownership_9056_scoped_forward_sink,
           forwarded_method_group_to_subscribe_spec_28_14) {
    auto r = runAotChecked(program(kHub,
        "      using var hub := new Hub(); "
        "      using var sub := hub.OnChanged(c.Hit); "
        "      hub.Fire(1.0); "));
    expectClean(r, repeatLine("1", 200));
}

// D — the forward to `EventSource<T>.Add` (stdlib §6.5: "scoped: Add stores"),
// the handler living until the source goes away with the hub.
BEZEL_TEST(ownership_9056_scoped_forward_sink,
           forwarded_lambda_to_add_spec_28_14) {
    auto r = runAotChecked(program(kHub,
        "      using var hub := new Hub(); "
        "      hub.On(lambda(t) => c.Hit(t)); "
        "      hub.Fire(1.0); "));
    expectClean(r, repeatLine("1", 200));
}

// E — the forwarding method belongs to a GENERIC class: its expression body is
// inlined at the call site, where the sink must still see the forwarded
// formal as the inlined method's own.
BEZEL_TEST(ownership_9056_scoped_forward_sink,
           forwarded_lambda_through_generic_hub_spec_28_14) {
    auto r = runAotChecked(program(
        "public class GHub<T> "
        "  private owned readonly _changed: EventSource<T> := new EventSource<T>(); "
        "  public method OnChanged(scoped handler: (T) -> ()): owned Subscription "
        "    => _changed.Subscribe(handler); "
        "  public method Fire(t: T) => _changed.Raise(t); "
        "end GHub; ",
        "      using var hub := new GHub<double>(); "
        "      using var sub := hub.OnChanged(lambda(t) => c.Hit(t)); "
        "      hub.Fire(1.0); "));
    expectClean(r, repeatLine("1", 200));
}

// F — FLIPPED by fix #9201 (accepted -> REJECTED). The forward
// `=> tok.Register(cb)` has a PARAMETER receiver: §28.14 form 4 needs a chain
// "rooted at `this` or a local binding", so form 5 resolves the receiver
// `tok` — "a parameter … admits no capture-bearing arguments" — which is not
// the callee's own bounding object, and the forward is not "to another
// `scoped` parameter whose bounding object is the same object" (§28.14,
// v0.359; ruling r43-rule-9201-9056 R4). The same callee is a measured ASan
// SEGV once the caller resolves the receiver instead of the handle (the
// #9201 row S2). Its block twin was already rejected; the arrow body now
// gets the same diagnostic. #9056's Register retain keeps its runtime oracle
// on the spec-legal form-3 spelling `_reg := tok.Register(cb)`
// (ownership_9274_scoped_forward_owned_chain.
// block_register_into_owned_field_admitted_spec_28_14_form_3).
BEZEL_TEST(ownership_9056_scoped_forward_sink,
           forwarded_lambda_to_token_register_spec_28_14) {
    auto r = runAot(
        "module P; "
        "import System.Threading; "
        "public class Box "
        "  private _v: int := 0; "
        "  public method Bump() => _v := _v + 1; "
        "  public property V: int => _v; "
        "end Box; "
        "public class Reg "
        "  public method OnCancel(tok: CancellationToken, scoped cb: () -> ()): "
        "    owned Subscription => tok.Register(cb); "
        "end Reg; "
        "public class Program "
        "  public static method Main() "
        "    for var i in 0..<200 do "
        "      using var box := new Box(); "
        "      using var r := new Reg(); "
        "      using var cts := new CancellationTokenSource(); "
        "      var tok := cts.Token; "
        "      using var sub := r.OnCancel(tok, lambda() => box.Bump()); "
        "      cts.Cancel(); "
        "      Console.WriteLine(box.V); "
        "    end; "
        "  end Main; "
        "end Program;");
    expectForwardRejected(r, "cb");
}

// G — a lambda literal forwarded TWICE through `this` (§28.14's "forward it
// to another `scoped` parameter of a call on `this`") into two `Add` sinks:
// each registration owns a reference of its own (2 hits per iteration).
BEZEL_TEST(ownership_9056_scoped_forward_sink,
           forwarded_lambda_added_twice_spec_28_14) {
    auto r = runAotChecked(program(
        "public class Hub "
        "  private owned readonly _changed: EventSource<double> := "
        "    new EventSource<double>(); "
        "  public method On(scoped handler: (double) -> ()) => _changed.Add(handler); "
        "  public method OnBoth(scoped handler: (double) -> ()) "
        "    this.On(handler); "
        "    this.On(handler); "
        "  end OnBoth; "
        "  public method Fire(t: double) => _changed.Raise(t); "
        "end Hub; ",
        "      using var hub := new Hub(); "
        "      hub.OnBoth(lambda(t) => c.Hit(t)); "
        "      hub.Fire(1.0); "));
    expectClean(r, repeatLine("2", 200));
}

// H — the caller-LOCAL spelling: a closure local passed at a `scoped` formal
// that the callee stores into a `scoped` field. The field store retains
// (#8817), so the local's +1 is the caller's to drop at its scope end; base
// treated the pass as an escape and never dropped it (a leaked closure per
// iteration).
BEZEL_TEST(ownership_9056_scoped_forward_sink,
           closure_local_passed_to_scoped_field_store_released_spec_28_14) {
    auto r = runAotChecked(program(
        "public class Keeper "
        "  private scoped _h: ((double) -> ())?; "
        "  public method Keep(scoped handler: (double) -> ()) => _h := handler; "
        "  public method Fire(t: double) "
        "    _h?(t); "
        "  end Fire; "
        "end Keeper; ",
        "      using var k := new Keeper(); "
        "      var h := lambda(t: double) => c.Hit(t); "
        "      k.Keep(h); "
        "      k.Fire(1.0); "));
    expectClean(r, repeatLine("1", 200));
}

// I — one closure LOCAL forwarded through a `scoped` formal to TWO
// subscriptions: each subscription owns a reference of its own, so the local's
// +1 stays the local's (2 hits per iteration). Base: both subscriptions took
// the local's single +1 as a transfer — `refcount over-release (closure)`.
BEZEL_TEST(ownership_9056_scoped_forward_sink,
           closure_local_forwarded_to_two_subscriptions_spec_28_14) {
    auto r = runAotChecked(program(kHub,
        "      using var hub := new Hub(); "
        "      var h := lambda(t: double) => c.Hit(t); "
        "      using var s1 := hub.OnChanged(h); "
        "      using var s2 := hub.OnChanged(h); "
        "      hub.Fire(1.0); "));
    expectClean(r, repeatLine("2", 200));
}

// J — a closure LOCAL passed at a `scoped` formal that the callee only
// INVOKES: nothing keeps it past the call, so the local's +1 is released at
// its own scope end. Base treated the pass as an escape and leaked the closure
// per iteration.
BEZEL_TEST(ownership_9056_scoped_forward_sink,
           closure_local_passed_to_invoking_scoped_formal_released_spec_28_14) {
    auto r = runAotChecked(program(
        "public class Once "
        "  public method Run(scoped handler: (double) -> ()) => handler(1.0); "
        "end Once; ",
        "      using var o := new Once(); "
        "      var h := lambda(t: double) => c.Hit(t); "
        "      o.Run(h); "));
    expectClean(r, repeatLine("1", 200));
}

// K — FLIPPED by fix #9201 (accepted -> REJECTED). The forward reaches the
// sink through the GETTER face `this.Changed` (stdlib §6.5's read-only
// `IEvent<T>` property): §28.14 form 4, "A chain containing … a getter …
// resolves no bounding object", and the transparent-getter lift is
// "recorded, not shipped" (v0.359; ruling r43-rule-9201-9056 R4). The block
// twin was already rejected. FLIPPED rather than respelled: the respelling
// `=> _changed.Subscribe(handler)` is row A's program verbatim.
BEZEL_TEST(ownership_9056_scoped_forward_sink,
           forwarded_lambda_to_ievent_subscribe_spec_28_14) {
    auto r = runAot(program(
        "public class Hub "
        "  private owned readonly _changed: EventSource<double> := "
        "    new EventSource<double>(); "
        "  public property Changed: IEvent<double> => _changed; "
        "  public method OnChanged(scoped handler: (double) -> ()): owned Subscription "
        "    => this.Changed.Subscribe(handler); "
        "  public method Fire(t: double) => _changed.Raise(t); "
        "end Hub; ",
        "      using var hub := new Hub(); "
        "      using var sub := hub.OnChanged(lambda(t) => c.Hit(t)); "
        "      hub.Fire(1.0); "));
    expectForwardRejected(r, "handler");
}

// CTRL1 — BOUNDARY CONTROL (base-green): a closure LOCAL forwarded to the
// same sink. A sink retain without the caller-local relief leaks the closure
// here; the relief without the sink retain over-releases it.
BEZEL_TEST(ownership_9056_scoped_forward_sink,
           control_closure_local_forwarded_to_subscribe_spec_28_14) {
    auto r = runAotChecked(program(kHub,
        "      using var hub := new Hub(); "
        "      var h := lambda(t: double) => c.Hit(t); "
        "      using var sub := hub.OnChanged(h); "
        "      hub.Fire(1.0); "));
    expectClean(r, repeatLine("1", 200));
}

// CTRL2 — BOUNDARY CONTROL (base-green): the DIRECT `src.Subscribe(lambda …)`,
// whose transferred +1 is the subscription's sole reference (#841). A retain
// that did not require a forwarded `scoped` formal leaks the closure here.
BEZEL_TEST(ownership_9056_scoped_forward_sink,
           control_direct_subscribe_lambda_spec_28_14) {
    auto r = runAotChecked(program("",
        "      using var src := new EventSource<double>(); "
        "      using var sub := src.Subscribe(lambda(t) => c.Hit(t)); "
        "      src.Raise(1.0); "));
    expectClean(r, repeatLine("1", 200));
}
