// Ownership / codegen — fix-forward #9056: the UNSUBSCRIBE half of the
// forwarded-`scoped`-handler discipline, and a conditional of two forwarded
// formals at the sink.
//
// Spec: stdlib §6.5 (`EventSource<T>.Remove(handler: (T) -> ()); //
// call-bounded: Remove only compares`), §17.2 (`source -= handler` is the
// fixed desugaring to `Remove`), §28.14 (scoped callback parameters; "or
// forward it to another `scoped` parameter"), §28.9 (each closure reference is
// released exactly once). Spec revision v0.359.
//
// Fix #9056 lets a caller's closure LOCAL passed to a forwarding `scoped`
// formal keep its own +1 (the sink takes a reference of its own), so the
// local's scope-end release must fire. A later `Src.Remove(f)` / `src -= f`
// was still classified as an ESCAPE of the local, which suppressed that
// release: the subscribe-then-unsubscribe idiom leaked one closure and its
// capture cells per iteration (rows A–D), and a bare `Src.Remove(f)` leaked
// the local even before #9056 (row E). The runtime's
// `bz_event_remove_closure` only compares and deactivates — it stores
// nothing — so the handler position is a call-bounded BORROW.
//
// Row F (FLIPPED to a rejection by fix #9201, see the row): a conditional
// whose arms are both `scoped` formals of the emitting method, handed to
// `Add`. The caller releases both lambda literals, so the
// sink must take its own reference on whichever arm is selected (a
// heap-use-after-free before).
//
// Every row is RED on its parent (r30-c1 + fix #9056; MEASURED with
// --no-results-cache) and GREEN with the fix-forward. Each loops 200x and
// prints the per-iteration hit count.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

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
    "  public method Ping() => _n := _n + 1; "
    "  public property N: int => _n; "
    "end Counter2; ";

const char* const kHub =
    "public class Hub "
    "  public owned readonly Src: EventSource<double> := "
    "    new EventSource<double>(); "
    "  public method On(scoped handler: (double) -> ()) => Src.Add(handler); "
    "  public method Off(handler: (double) -> ()) => Src.Remove(handler); "
    "  public method Fire(t: double) => Src.Raise(t); "
    "end Hub; ";

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

}  // namespace

// A — a closure local forwarded to `On` (a `scoped` formal stored by `Add`),
// then removed directly from the source; the second Raise must not fire it.
BEZEL_TEST(ownership_9056_ff_remove_borrow,
           closure_local_forwarded_then_removed_stdlib_6_5) {
    auto r = runAotChecked(program(kHub,
        "      using var hub := new Hub(); "
        "      var f := lambda(t: double) => c.Hit(t); "
        "      hub.On(f); "
        "      hub.Fire(1.0); "
        "      hub.Src.Remove(f); "
        "      hub.Fire(1.0); "));
    expectClean(r, repeatLine("1", 200));
}

// B — the same, removed through a plain (call-bounded) helper formal.
BEZEL_TEST(ownership_9056_ff_remove_borrow,
           closure_local_forwarded_then_removed_via_helper_stdlib_6_5) {
    auto r = runAotChecked(program(kHub,
        "      using var hub := new Hub(); "
        "      var f := lambda(t: double) => c.Hit(t); "
        "      hub.On(f); "
        "      hub.Fire(1.0); "
        "      hub.Off(f); "
        "      hub.Fire(1.0); "));
    expectClean(r, repeatLine("1", 200));
}

// C — the same through a GENERIC hub (its expression bodies are inlined).
BEZEL_TEST(ownership_9056_ff_remove_borrow,
           closure_local_forwarded_then_removed_generic_hub_stdlib_6_5) {
    auto r = runAotChecked(program(
        "public class GHub<T> "
        "  public owned readonly Src: EventSource<T> := new EventSource<T>(); "
        "  public method On(scoped handler: (T) -> ()) => Src.Add(handler); "
        "  public method Fire(t: T) => Src.Raise(t); "
        "end GHub; ",
        "      using var hub := new GHub<double>(); "
        "      var f := lambda(t: double) => c.Hit(t); "
        "      hub.On(f); "
        "      hub.Fire(1.0); "
        "      hub.Src.Remove(f); "
        "      hub.Fire(1.0); "));
    expectClean(r, repeatLine("1", 200));
}

// D — the `-=` desugaring (spec §17.2), inside the declaring class's
// plain-formal helper: the caller's closure local is removed through it.
BEZEL_TEST(ownership_9056_ff_remove_borrow,
           closure_local_forwarded_then_minus_assigned_via_helper_spec_17_2) {
    auto r = runAotChecked(program(
        "public class Hub "
        "  private owned readonly _src: EventSource<double> := "
        "    new EventSource<double>(); "
        "  public method On(scoped handler: (double) -> ()) => _src.Add(handler); "
        "  public method Off(handler: (double) -> ()) "
        "    _src -= handler; "
        "  end Off; "
        "  public method Fire(t: double) => _src.Raise(t); "
        "end Hub; ",
        "      using var hub := new Hub(); "
        "      var f := lambda(t: double) => c.Hit(t); "
        "      hub.On(f); "
        "      hub.Fire(1.0); "
        "      hub.Off(f); "
        "      hub.Fire(1.0); "));
    expectClean(r, repeatLine("1", 200));
}

// E — `Src.Remove(f)` alone (nothing registered): the local is borrowed by
// the comparison and released at its own scope end. Leaked before #9056 too.
BEZEL_TEST(ownership_9056_ff_remove_borrow,
           closure_local_removed_only_released_stdlib_6_5) {
    auto r = runAotChecked(program(kHub,
        "      using var hub := new Hub(); "
        "      var f := lambda(t: double) => c.Hit(t); "
        "      hub.Src.Remove(f); "));
    expectClean(r, repeatLine("0", 200));
}

// F — FLIPPED by fix #9201 (accepted -> REJECTED), under the conservative v1
// reading (ruling r43-rule-9201-9056 §4 item 3). §28.14 lets the callee
// "forward it to another `scoped` parameter": the forward names the
// PARAMETER, and a conditional `(if pick then a else b)` reads each formal as
// a value. The block twin was already rejected (probe S4); the arrow body now
// gets the same diagnostics. #9056-ff's conditional retain in codegen stays
// (harmless; unreachable from user code until the bound is lifted).
BEZEL_TEST(ownership_9056_ff_remove_borrow,
           conditional_of_two_scoped_formals_forwarded_to_add_spec_28_14) {
    auto r = runAot(program(
        "public class Hub "
        "  private owned readonly _changed: EventSource<double> := "
        "    new EventSource<double>(); "
        "  public method On(scoped a: (double) -> (), scoped b: (double) -> (), "
        "                   pick: bool) => _changed.Add((if pick then a else b)); "
        "  public method Fire(t: double) => _changed.Raise(t); "
        "end Hub; ",
        "      using var hub := new Hub(); "
        "      hub.On(lambda(t) => c.Hit(t), lambda(t) => c.Ping(), i mod 2 = 0); "
        "      hub.Fire(1.0); "));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    for (const char* name : {"a", "b"}) {
        const std::string msg =
            std::string("`") + name +
            "` is a `scoped` parameter — it may only be invoked, stored into "
            "a `scoped` field of `this`, or forwarded to a `scoped` parameter "
            "of a call on `this`; it may not be read as a value (§28.14, "
            "v0.175)";
        BEZEL_EXPECT_TRUE(
            bezel::test::diagnosticMessageContains(r, msg.c_str()));
    }
}
