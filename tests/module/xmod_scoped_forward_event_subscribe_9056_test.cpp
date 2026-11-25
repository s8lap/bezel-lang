// Module / codegen — fix #9056 across a module boundary. The two-module twins
// of tests/ownership/scoped_forward_event_subscribe_9056_test.cpp: a library
// method forwards its `scoped` handler formal into a storing stdlib sink
// (`EventSource<T>.Subscribe`), or stores it into a `scoped` field, and an
// importing module calls it.
//
// Spec: §28.14 (scoped callback parameters), §28.9 (the closure object is
// released exactly once), §28.3 (a by-value function-typed parameter is a
// BORROW), §2.5 (an imported member is described by the exporter's
// metadata; a GENERIC type's body is re-parsed and compiled in the
// importer), stdlib §6.5. Spec revision v0.359.
//
// The exporter compiles the forwarding method under the same discipline as a
// single-module program: its sink takes a reference of its own on a forwarded
// `scoped` formal, and its `scoped` field store retains (#8817). So the
// importer's caller must release its own `+1` producer — for a non-generic
// import (no `ast::MethodDecl`) as for a local callee. Base never did, and
// for an imported GENERIC method (whose body is inlined in the importer) the
// sink inside the inlined body took the caller's reference as a transfer
// while the caller released it.
//
// ROWS A and B are RED on r30-c1 @ 21cc495b2 (MEASURED with
// --no-results-cache: A a heap-use-after-free in `main`; B a leaked closure
// per iteration) and GREEN with the fix. CTRL1 and CTRL2 are the suite's two
// BOUNDARY CONTROLS (base-green): the non-generic forward to `Add` with a
// lambda literal and with a closure local — an exporter-side sink retain that
// the importer's caller does not match by a release leaks both. (`Add`, not
// `Subscribe`: a `using var` bound to an IMPORTED method's `owned
// Subscription` result has no disposal arm and leaks the Subscription on base
// and tip alike, #6918 — a separate defect.)

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

const char* const kLib9056 =
    "module HubLib9056; "
    "public class Hub "
    "  private owned readonly _changed: EventSource<double> := "
    "    new EventSource<double>(); "
    "  public method OnChanged(scoped handler: (double) -> ()): owned Subscription "
    "    => _changed.Subscribe(handler); "
    "  public method On(scoped handler: (double) -> ()) => _changed.Add(handler); "
    "  public method Fire(t: double) => _changed.Raise(t); "
    "end Hub; "
    "public class GHub<T> "
    "  private owned readonly _changed: EventSource<T> := new EventSource<T>(); "
    "  public method OnChanged(scoped handler: (T) -> ()): owned Subscription "
    "    => _changed.Subscribe(handler); "
    "  public method Fire(t: T) => _changed.Raise(t); "
    "end GHub; "
    "public class Keeper "
    "  private scoped _h: ((double) -> ())?; "
    "  public method Keep(scoped handler: (double) -> ()) => _h := handler; "
    "  public method Fire(t: double) "
    "    _ := _h?(t); "
    "  end Fire; "
    "end Keeper; ";

// A 200-iteration importing Main printing the per-iteration hit count.
std::string app9056(const std::string& body) {
    return "module App9056; "
           "import HubLib9056; "
           "public class Counter2 "
           "  private _n: int := 0; "
           "  public method Hit(t: double) => _n := _n + 1; "
           "  public property N: int => _n; "
           "end Counter2; "
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

std::string ones200() {
    std::string out;
    for (int i = 0; i < 200; ++i) out += "1\n";
    return out;
}

}  // namespace

// A — an imported GENERIC hub: its expression body is inlined in the importer,
// where the sink must take its own reference on the forwarded formal.
BEZEL_TEST(xmod_9056_scoped_forward_sink,
           imported_generic_forward_to_subscribe_spec_28_14) {
    auto r = test::runAotModulesChecked(
        {{"HubLib9056", kLib9056},
         {"App9056", app9056(
             "      using var hub := new GHub<double>(); "
             "      using var sub := hub.OnChanged(lambda(t) => c.Hit(t)); "
             "      hub.Fire(1.0); ")}},
        "App9056");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, ones200());
}

// B — an imported non-generic `scoped` field store: the exporter's store
// retains (#8817), so the importer's lambda literal is the caller's to drop.
BEZEL_TEST(xmod_9056_scoped_forward_sink,
           imported_scoped_field_store_releases_caller_producer_spec_28_14) {
    auto r = test::runAotModulesChecked(
        {{"HubLib9056", kLib9056},
         {"App9056", app9056(
             "      using var k := new Keeper(); "
             "      k.Keep(lambda(t) => c.Hit(t)); "
             "      k.Fire(1.0); ")}},
        "App9056");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, ones200());
}

// CTRL1 — BOUNDARY CONTROL (base-green): the non-generic imported forward to
// `EventSource<T>.Add` with a lambda literal.
BEZEL_TEST(xmod_9056_scoped_forward_sink,
           control_imported_forward_lambda_to_add_spec_28_14) {
    auto r = test::runAotModulesChecked(
        {{"HubLib9056", kLib9056},
         {"App9056", app9056(
             "      using var hub := new Hub(); "
             "      hub.On(lambda(t) => c.Hit(t)); "
             "      hub.Fire(1.0); ")}},
        "App9056");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, ones200());
}

// CTRL2 — BOUNDARY CONTROL (base-green): the same forward with a closure
// LOCAL, which keeps its +1 and releases it at its own scope end.
BEZEL_TEST(xmod_9056_scoped_forward_sink,
           control_imported_forward_local_to_add_spec_28_14) {
    auto r = test::runAotModulesChecked(
        {{"HubLib9056", kLib9056},
         {"App9056", app9056(
             "      using var hub := new Hub(); "
             "      var h := lambda(t: double) => c.Hit(t); "
             "      hub.On(h); "
             "      hub.Fire(1.0); ")}},
        "App9056");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, ones200());
}

}  // namespace bezel::module
