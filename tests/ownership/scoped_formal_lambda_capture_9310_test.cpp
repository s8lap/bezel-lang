// Regression tests for issue #9310 — §28.14 *The `scoped` modifier* (v0.359),
// the CALLEE side, over a CLOSURE that captures the method's own `scoped`
// function-typed parameter.
//
// Spec: §28.14 (v0.359): "The callee may store the callback, but only in
// `scoped` fields of the bounding object itself (below), or forward it to
// another `scoped` parameter whose bounding object is the same object; it may
// invoke it. It may not return it, move it into non-`scoped` storage, or store
// it in any other object. … These obligations are compiler-verified for
// user-authored callees." §28.3's closure bullet: a closure that captures the
// formal carries it in its source set. §28.14 forms 4/5: a bounding object
// that resolves to none of the admitted forms (a static chain, a getter, a
// borrow-field hop, a parameter receiver) "admits no capture-bearing
// arguments".
//
// THE DEFECT. The callee walk read only a BARE use of the formal. The direct
// `_keep.bus.Add(handler)` was rejected (the forward diagnostic), but the
// closure spelling `_keep.bus.Add(lambda(t: double) => handler(t))` was
// admitted, and the program died with an AddressSanitizer SEGV when the stored
// closure later ran (the registered closure dangled over the caller's
// `Counter2`).
//
// ROWS. One reject row per sink — the issue's static owned chain, a plain
// field, a collection, an `event +=`, a returned closure (block and arrow), a
// nested lambda, a local alias, and a getter face — each asserting the
// SPECIFIC diagnostic. Two boundary CONTROLS keep the legal shapes admitted
// and running: the closure forwarded to a `scoped` position on an owned-field
// chain of `this`, and a closure that only invokes the formal locally. The
// remaining controls are in the conformance corpus.
//
// ORACLE. `runAot` / `runAotChecked`.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The leading words every #9310 diagnostic shares.
const char* kCaptures =
    "this closure captures `handler`, a `scoped` parameter";

// A `scoped` position whose bounding object resolves to nothing.
const char* kNoBound =
    "is passed to a `scoped` position whose bounding object resolves to "
    "neither `this` nor a `using var` local";

const char* kSubscribeNoBound =
    "is subscribed with `+=` (`.Add`, a `scoped` position) on a receiver "
    "whose bounding object resolves to neither `this` nor a `using var` local";

const char* kStoredInField =
    "so it may not be stored into a field, a static field, an indexer slot "
    "or an `out`/`ref` parameter";

const char* kStoredInCollection =
    "so it may not be stored into a collection reachable beyond the call";

const char* kReturned = "so it may not be returned";

const char* const kPrelude =
    "module P; "
    "public class Counter2 "
    "  private _n: int := 0; "
    "  public method Hit(t: double) => _n := _n + 1; "
    "  public property N: int => _n; "
    "end Counter2; "
    "public class Holder "
    "  public owned readonly bus: EventSource<double> := "
    "    new EventSource<double>(); "
    "  public method Fire(t: double) => bus.Raise(t); "
    "end Holder; ";

// The issue's driver: register a capturing lambda through `hub.On` inside a
// `do` block, then fire after the block closed.
const char* const kDriver =
    "public class Program "
    "  public static method Main() "
    "    for var i in 0..<200 do "
    "      do "
    "        using var c := new Counter2(); "
    "        using var hub := new Hub(); "
    "        hub.On(lambda(t) => c.Hit(t)); "
    "      end; "
    "      Hub.Fire(1.0); "
    "    end; "
    "    Console.WriteLine(\"done\"); "
    "  end Main; "
    "end Program;";

// A Hub with the issue's static owned chain and `member` as its `On`.
std::string hub(const std::string& fields, const std::string& member) {
    return std::string(kPrelude) +
           "public class Hub "
           "  private static owned readonly _keep: Holder := new Holder(); "
           "  private owned readonly _changed: EventSource<double> := "
           "    new EventSource<double>(); " +
           fields +
           "  public static method Fire(t: double) => _keep.Fire(t); "
           "  public property Changed: IEvent<double> => _changed; " +
           member + "end Hub; " + kDriver;
}

void expectReject(const JitResult& r, const char* diagnostic) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kCaptures));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, diagnostic));
}

}  // namespace

// 1 — THE ISSUE'S REPRO, verbatim: the closure is stored in a STATIC owned
// chain. RED on the base (admitted; SEGVs at run time).
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           lambda_over_scoped_formal_into_static_owned_chain_rejected_spec_28_14) {
    auto r = runAot(hub("",
                        "  public method On(scoped handler: (double) -> ()) "
                        "    _keep.bus.Add(lambda(t: double) => handler(t)); "
                        "  end On; "));
    expectReject(r, kNoBound);
}

// 2 — the `+=` spelling of the same store (§17.2: `lhs += h` is
// `lhs.Add(h)`).
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           lambda_over_scoped_formal_plus_eq_on_static_chain_rejected_spec_28_14_spec_17_2) {
    auto r = runAot(hub("",
                        "  public method On(scoped handler: (double) -> ()) "
                        "    _keep.bus += lambda(t: double) => handler(t); "
                        "  end On; "));
    expectReject(r, kSubscribeNoBound);
}

// 3 — the closure stored in a PLAIN field: "move it into non-`scoped`
// storage".
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           lambda_over_scoped_formal_stored_in_plain_field_rejected_spec_28_14) {
    auto r = runAot(hub("  private _f: ((double) -> ())? := null; ",
                        "  public method On(scoped handler: (double) -> ()) "
                        "    _f := lambda(t: double) => handler(t); "
                        "  end On; "));
    expectReject(r, kStoredInField);
}

// 4 — the closure stored in a collection reachable beyond the call.
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           lambda_over_scoped_formal_added_to_field_collection_rejected_spec_28_14) {
    auto r = runAot(
        hub("  private owned _list: Array<(double) -> ()> := []; ",
            "  public method On(scoped handler: (double) -> ()) "
            "    _list.Add(lambda(t: double) => handler(t)); "
            "  end On; "));
    expectReject(r, kStoredInCollection);
}

// 5 — a RETURNED closure (block body): "It may not return it".
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           lambda_over_scoped_formal_returned_block_rejected_spec_28_14) {
    auto r = runAot(
        hub("",
            "  public method Wrap(scoped handler: (double) -> ()): "
            "      (double) -> () "
            "    return lambda(t: double) => handler(t); "
            "  end Wrap; "
            "  public method On(scoped handler: (double) -> ()) "
            "  end On; "));
    expectReject(r, kReturned);
}

// 6 — the arrow-bodied twin: the `=> E` body IS the returned value.
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           lambda_over_scoped_formal_returned_arrow_rejected_spec_28_14) {
    auto r = runAot(
        hub("",
            "  public method Wrap(scoped handler: (double) -> ()): "
            "      (double) -> () => lambda(t: double) => handler(t); "
            "  public method On(scoped handler: (double) -> ()) "
            "  end On; "));
    expectReject(r, kReturned);
}

// 7 — a NESTED lambda: the stored closure captures the formal through an
// inner closure of its own.
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           nested_lambda_over_scoped_formal_into_static_chain_rejected_spec_28_14) {
    auto r = runAot(
        hub("",
            "  public method On(scoped handler: (double) -> ()) "
            "    _keep.bus.Add(lambda(t: double) => do "
            "      var g := lambda(u: double) => handler(u); "
            "      g(t); "
            "    end); "
            "  end On; "));
    expectReject(r, kNoBound);
}

// 8 — a LOCAL alias: the closure is bound to a local first; the local
// carries the formal, so its later store is the same store.
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           local_alias_of_lambda_over_scoped_formal_stored_rejected_spec_28_14) {
    auto r = runAot(
        hub("  private _f: ((double) -> ())? := null; ",
            "  public method On(scoped handler: (double) -> ()) "
            "    var f := lambda(t: double) => handler(t); "
            "    _f := f; "
            "  end On; "));
    expectReject(r, kStoredInField);
}

// 9 — a GETTER-face receiver (`this.Changed.Subscribe(...)`): §28.14 form 4
// "a chain containing … a getter … resolves no bounding object".
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           lambda_over_scoped_formal_into_getter_face_rejected_spec_28_14) {
    auto r = runAot(
        hub("",
            "  public method On(scoped handler: (double) -> ()) "
            "    this.Changed.Add(lambda(t: double) => handler(t)); "
            "  end On; "));
    expectReject(r, kNoBound);
}

// CONTROL 1 — the legal forward: the closure goes to a `scoped` position on an
// owned-field chain of `this` (form 4 → `this`), the closure twin of the
// admitted `_changed.Add(handler)` (§28.14 / #9274). Must stay admitted and
// run clean: the caller's `Counter2` is bounded by `hub`.
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           lambda_over_scoped_formal_into_owned_field_chain_admitted_spec_28_14_form_4) {
    // `runAot`, not `runAotChecked`: the closure the callee builds here takes a
    // 24-byte cell that nothing releases (a separate, pre-existing closure
    // leak, #9325) — this row pins ADMISSION and the hit counts.
    auto r = runAot(std::string(kPrelude) +
                           "public class Hub "
                           "  private owned readonly _changed: "
                           "    EventSource<double> := "
                           "    new EventSource<double>(); "
                           "  public method Fire(t: double) => _changed.Raise(t); "
                           "  public method On(scoped handler: (double) -> ()) "
                           "    _changed.Add(lambda(t: double) => handler(t)); "
                           "  end On; "
                           "end Hub; "
                           "public class Program "
                           "  public static method Main() "
                           "    for var i in 0..<200 do "
                           "      using var c := new Counter2(); "
                           "      using var hub := new Hub(); "
                           "      hub.On(lambda(t) => c.Hit(t)); "
                           "      hub.Fire(1.0); "
                           "      Console.WriteLine(c.N); "
                           "    end; "
                           "  end Main; "
                           "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string expected;
    for (int i = 0; i < 200; ++i) expected += "1\n";
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// CONTROL 2 — a closure that only INVOKES the formal, bound to a local and
// called locally: nothing stores it, so it must stay admitted.
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           lambda_over_scoped_formal_invoked_locally_admitted_spec_28_14) {
    auto r = runAotChecked(std::string(kPrelude) +
                           "public class Hub "
                           "  public method On(scoped handler: (double) -> ()) "
                           "    var f := lambda(t: double) => handler(t); "
                           "    f(1.0); "
                           "    f(2.0); "
                           "  end On; "
                           "end Hub; "
                           "public class Program "
                           "  public static method Main() "
                           "    using var c := new Counter2(); "
                           "    using var hub := new Hub(); "
                           "    hub.On(lambda(t) => c.Hit(t)); "
                           "    Console.WriteLine(c.N); "
                           "  end Main; "
                           "end Program;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// ---------------------------------------------------------------------------
// fix-forward #9310 — the carrier LOCAL is traced through the §28.3 flow
// facts, not in walk order.
//
// §28.3 *Provenance* clause 1 (v0.359): a captured binding holds "every value
// it is ever assigned … whatever the assignment's position relative to the
// capture or to the read"; "a binding no lambda captures contributes the
// assignments that reach the read". The first commit recorded a carrier local
// in WALK order and never un-recorded it, so (a) a carrier that reaches a sink
// only around a loop's BACK EDGE was admitted (and SEGVd), and (b) a local
// overwritten by a capture-free lambda before the sink was still rejected.

// The static-chain store sink the back-edge rows use: a plain field of an
// object held in a static owned field.
namespace {
std::string fboxHub(const std::string& member) {
    return std::string(kPrelude) +
           "public class FBox "
           "  public f: ((double) -> ())? := null; "
           "  public method Fire(t: double) => f?(t); "
           "end FBox; "
           "public class Hub "
           "  private static owned readonly _keep: FBox := new FBox(); "
           "  public static method Fire(t: double) => _keep.Fire(t); " +
           member + "end Hub; " + kDriver;
}
}  // namespace

// 10 — the reviewer's h15: the store PRECEDES `f := <carrier>` in the loop
// body, so the carrier reaches it on the second iteration, around the back
// edge. RED on the first commit (admitted; ASan SEGV in `Counter2$Hit`).
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           carrier_local_reaching_static_store_around_loop_back_edge_rejected_spec_28_14_spec_28_3) {
    auto r = runAot(fboxHub(
        "  public method On(scoped handler: (double) -> ()) "
        "    var f := lambda(t: double) => Console.Write(\"\"); "
        "    for var i in 0..<2 do "
        "      _keep.f := f; "
        "      f := lambda(t: double) => handler(t); "
        "    end; "
        "  end On; "));
    expectReject(r, kStoredInField);
}

// 11 — the same back edge through a SECOND local and a `while` loop: `g`
// takes `f`'s carrier only after `f` is assigned it, so the union is a fixed
// point over the body, not a single walk.
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           carrier_reaching_static_store_through_alias_around_while_back_edge_rejected_spec_28_14_spec_28_3) {
    auto r = runAot(fboxHub(
        "  public method On(scoped handler: (double) -> ()) "
        "    var f := lambda(t: double) => Console.Write(\"\"); "
        "    var g := f; "
        "    var i := 0; "
        "    while i < 3 do "
        "      _keep.f := g; "
        "      g := f; "
        "      f := lambda(t: double) => handler(t); "
        "      i := i + 1; "
        "    end; "
        "  end On; "));
    expectReject(r, kStoredInField);
}

// 12 — the reviewer's h13: `f` is captured by no lambda, and the only
// assignment that reaches `_f := f` is the capture-free lambda, so the stored
// value carries nothing (§28.3 clause 1, "the assignments that reach the
// read"). RED on the first commit (rejected as a carrier store); legal.
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           uncaptured_local_overwritten_by_capture_free_lambda_then_stored_admitted_spec_28_3_clause_1) {
    // `runAot`, not `runAotChecked`: a closure LOCAL stored into a
    // function-typed field leaks its closure cell (pre-existing, #9143; the
    // capture-free `var f := lambda ...; _f := f;` leaks the same on the
    // base) — this row pins ADMISSION and the output.
    auto r = runAot(std::string(kPrelude) +
                    "public class Hub "
                    "  private _f: ((double) -> ())? := null; "
                    "  public method Fire(t: double) => _f?(t); "
                    "  public method On(scoped handler: (double) -> ()) "
                    "    var f := lambda(t: double) => handler(t); "
                    "    f(1.0); "
                    "    f := lambda(t: double) => Console.Write(\"\"); "
                    "    _f := f; "
                    "  end On; "
                    "end Hub; "
                    "public class Program "
                    "  public static method Main() "
                    "    for var i in 0..<200 do "
                    "      using var c := new Counter2(); "
                    "      using var hub := new Hub(); "
                    "      hub.On(lambda(t) => c.Hit(t)); "
                    "      hub.Fire(1.0); "
                    "      if i = 199 then Console.WriteLine(c.N); end; "
                    "    end; "
                    "    Console.WriteLine(\"done\"); "
                    "  end Main; "
                    "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\ndone\n"));
}

// 13 — the loop twin of row 12: inside the loop body the capture-free `:=`
// precedes the store and so cuts the carrier off on the forward path AND
// around the back edge (the kill sits in the loop and dominates the read).
// RED on the first commit (rejected); legal.
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           uncaptured_local_killed_inside_loop_before_static_store_admitted_spec_28_3_clause_1) {
    // `runAot`, not `runAotChecked`: a closure LOCAL stored into a
    // function-typed field leaks its closure cell (pre-existing, #9143; the
    // capture-free `var f := lambda ...; _f := f;` leaks the same on the
    // base) — this row pins ADMISSION and the output.
    auto r = runAot(fboxHub(
        "  public method On(scoped handler: (double) -> ()) "
        "    var f := lambda(t: double) => Console.Write(\"\"); "
        "    for var i in 0..<2 do "
        "      f := lambda(t: double) => handler(t); "
        "      f(1.0); "
        "      f := lambda(t: double) => Console.Write(\"\"); "
        "      _keep.f := f; "
        "    end; "
        "  end On; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("done\n"));
}

// 14 — TRANSITIVE: the stored closure captures a carrier LOCAL, not the
// formal (§28.3 clause 1, "a captured function value its own set,
// transitively"). RED on the first commit (admitted), whose lambda arm read
// only a captured `scoped` formal.
BEZEL_TEST(scoped_formal_lambda_capture_9310,
           lambda_capturing_carrier_local_into_static_store_rejected_spec_28_14_spec_28_3) {
    auto r = runAot(fboxHub(
        "  public method On(scoped handler: (double) -> ()) "
        "    var f := lambda(t: double) => handler(t); "
        "    _keep.f := lambda(t: double) => f(t); "
        "  end On; "));
    expectReject(r, kStoredInField);
}
