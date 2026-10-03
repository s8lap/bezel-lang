// fix #4972 — codegen: capture-free lambda handler on
// ISharedEvent<T> / SharedEventSource<T> (§6.5 v0.307 / §28.14 v0.307).
//
// The #4901 codegen narrow rejected EVERY handler shape that was not a
// STATIC non-overloaded `(T) -> ()` method reference; a capture-free
// lambda was therefore a spurious reject at every subscribe position
// (Subscribe / Add / `+=`) — though stdlib §6.5 v0.307 admits "a
// non-capturing lambda or a static method reference" everywhere the
// static-method-ref form is admitted, and #4920 (sema) already admits
// the capture-free LAMBDA LITERAL. This file is the codegen-LIFT
// regression pin: the spec-stated behaviour, asserted with VALUE
// outcomes on every subscribe position.
//
// FAIL-WITHOUT proof: the pre-fix `b640eb0d` (or earlier) compiler
// rejects EVERY row below with the codegen `+=` gate's own diagnostic
// (the `+=` row) or the #2457 "produced no value" backstop (Subscribe /
// Add). The post-fix compiler runs every row to `exitCode == 0` with
// `stdoutText == "9\n"`. The semantic spec-stated outcome is "the
// handler runs, prints 9, and exits 0"; no row asserts a mere "did
// not reject" — every row pins the printed value, so the test cannot
// vacuously pass on a regression that compiles but does not run the
// handler.
//
// Capturing-handler regression coverage is owned by
// tests/sema/shared_event_capture_free_4920_test.cpp (#4920 is the
// sema gate). Capturing / overload-group / wrong-payload / non-unit-
// return rejection coverage for codegen lives in
// tests/codegen/shared_event_call_lowering_4901_test.cpp.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Value oracle: the handler runs, prints 9, exits 0.
bool lambdaHandlerLowers(const std::string& program) {
    auto r = runAot(program);
    if (!(r.ran && r.exitCode == 0)) return false;
    return r.stdoutText == "9\n" || r.stdoutText == "9\r\n";
}

}  // namespace

// ------------------------------------------------------------------------
// The issue body — exactly: `Bus.Feed += lambda(x: int) => Console.WriteLine(x);`
// ------------------------------------------------------------------------
BEZEL_TEST(codegen_shared_event_lambda_handler_4972,
           plus_eq_capture_free_lambda_spec_6_5_28_14) {
    BEZEL_EXPECT_TRUE(lambdaHandlerLowers(
        "module M; "
        "import System.Threading; "
        "class Bus "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "end Bus; "
        "class C "
        "  public static method Main(): int "
        "    Bus.Feed += lambda(x: int) => Console.WriteLine(x); "
        "    Bus.Feed.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// ------------------------------------------------------------------------
// Subscribe — `var sub := Feed.Subscribe(lambda ...)`; dispose after.
// ------------------------------------------------------------------------
BEZEL_TEST(codegen_shared_event_lambda_handler_4972,
           subscribe_capture_free_lambda_spec_6_5_28_14) {
    BEZEL_EXPECT_TRUE(lambdaHandlerLowers(
        "module M; "
        "import System.Threading; "
        "class Bus "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "end Bus; "
        "class C "
        "  public static method Main(): int "
        "    var sub := Bus.Feed.Subscribe("
        "        lambda(x: int) => Console.WriteLine(x)); "
        "    Bus.Feed.Raise(9); "
        "    sub.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// ------------------------------------------------------------------------
// Add — `Feed.Add(lambda ...)`; subscription is alive for the program
// lifetime (this test exits at end of Main after one Raise, so no Dispose
// is necessary).
// ------------------------------------------------------------------------
BEZEL_TEST(codegen_shared_event_lambda_handler_4972,
           add_capture_free_lambda_spec_6_5_28_14) {
    BEZEL_EXPECT_TRUE(lambdaHandlerLowers(
        "module M; "
        "import System.Threading; "
        "class Bus "
        "  public static owned readonly Feed: SharedEventSource<int> := new(); "
        "end Bus; "
        "class C "
        "  public static method Main(): int "
        "    Bus.Feed.Add(lambda(x: int) => Console.WriteLine(x)); "
        "    Bus.Feed.Raise(9); "
        "    return 0; "
        "  end Main; "
        "end C;"));
}

// ------------------------------------------------------------------------
// Subscribe on the ISharedEvent<T> interface FACE (a static property of
// interface type) — exercises the new ISharedEvent<T> arm added in #4901
// with a lambda argument rather than a static method reference. Raise
// from the declaring class through the backing SharedEventSource<T>
// field.
// ------------------------------------------------------------------------
BEZEL_TEST(codegen_shared_event_lambda_handler_4972,
           subscribe_via_ishared_event_face_capture_free_lambda_spec_6_5_28_14) {
    BEZEL_EXPECT_TRUE(lambdaHandlerLowers(
        "module M; "
        "import System.Threading; "
        "class Bus "
        "  private static owned readonly _feed: SharedEventSource<int> := new(); "
        "  public static property Feed: ISharedEvent<int> => _feed; "
        "  public static method Publish(x: int) _feed.Raise(x); end Publish; "
        "end Bus; "
        "class C "
        "  public static method Main(): int "
        "    var sub := Bus.Feed.Subscribe("
        "        lambda(x: int) => Console.WriteLine(x)); "
        "    Bus.Publish(9); "
        "    sub.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;"));
}