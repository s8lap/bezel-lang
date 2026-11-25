// Regression tests for issue #9353 — §28.3 *Function-typed holders* and
// §28.14 (v0.359): a capture-bearing closure over a `scoped` (or a plain,
// call-bounded) function-typed formal admitted as the FILL argument of
// `new Array<F>(size, fill)`, then read out by index and stored into a static
// field.
//
// Spec: §28.3 *Function-typed holders* (v0.359): "A **function-typed holder**
// is a field (a `scoped` field excepted), a record component, a choice case's
// payload field, an inline-array slot, … and **any collection slot**. At every
// function-typed holder the closure bullet's sink rule holds: a store of a
// function value admits a capture-free value and rejects one whose source set
// is non-empty (a capture-bearing closure, **a plain function-typed
// parameter**, a local carrying either), judged at the site". A function value
// READ from a holder carries the empty set (*Provenance* clause 5), so the
// closure handed to `arr[0]` has already lost its bound. §28.14: the callee
// "may not … store it in any other object", and for the call-bounded tier
// "stored into any collection, a local one included". The standard library
// declares the constructor as `Array<T>(size: int, fill: T)` (stdlib §4.4) —
// "Count = size, every slot = fill" — so the `fill` argument is a store into
// every slot of the new array.
//
// THE DEFECT. The mutator spellings of the same local-collection store
// (`arr.Add(c)`, a collection-literal element) are rejected by the §28.3
// holder rule; the CONSTRUCTOR's fill argument was not, so the closure was
// admitted, stored into every slot, read back by index with the empty source
// set, and stored into a static field — the program died with an
// AddressSanitizer SEGV / heap-use-after-free when the stored closure ran
// (`#0 bz$P.Counter2$Hit #1 lam$1 #2 lam$0`).
//
// ROWS. One reject row per binding shape of the fill argument — the
// positional argument, the NAMED argument, and a carrier LOCAL as the
// argument ("a local carrying either") — for the `scoped` formal, and the
// same positional shape for the call-bounded formal. The admitting twins (a
// capture-free fill, for both formal kinds) are in `corpus-candidates/`, not
// here: they are green on the base.
//
// ORACLE. `runAot` (the neighbours' oracle for a compile-time rejection).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The `scoped` tier's diagnostic (the #9310 carrier walk).
const char* kCapturesScoped =
    "this closure captures `handler`, a `scoped` parameter";
const char* kStoredInCollection =
    "may not be stored into a collection, a local one included";

// The §28.14 call-bounded tier's diagnostic.
const char* kCallBounded = "parameter `handler` is call-bounded";
const char* kStoredIntoCollection = "may not be stored into a collection slot";

const char* const kPrelude =
    "module P; "
    "public class Counter2 "
    "  private _n: int := 0; "
    "  public method Hit(t: double) => _n := _n + 1; "
    "  public property N: int => _n; "
    "end Counter2; "
    "public class FBox "
    "  public f: ((double) -> ())? := null; "
    "  public method Fire(t: double) => f?(t); "
    "end FBox; ";

// The issue's driver: hand `On` a closure over a `Counter2` that dies with
// the block, then fire the stored callback after the block closed.
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
std::string fboxHub(const std::string& member) {
    return std::string(kPrelude) +
           "public class Hub "
           "  private static owned readonly _keep: FBox := new FBox(); "
           "  public static method Fire(t: double) => _keep.Fire(t); " +
           member + "end Hub; " + kDriver;
}

// The `scoped` tier's rejection.
void expectScopedReject(const JitResult& r) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kCapturesScoped));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, kStoredInCollection));
}

// The §28.14 call-bounded tier's rejection.
void expectCallBoundedReject(const JitResult& r) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kCallBounded));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, kStoredIntoCollection));
}

}  // namespace

// 1 — THE ISSUE'S REPRO, verbatim: the closure is the FILL argument of
// `new Array<(double) -> ()>(1, …)`, is stored into every slot, and is read
// back by index and stored into a static field. RED on the base (admitted; the
// stored closure SEGVs at run time).
BEZEL_TEST(array_fill_capture_9353,
           lambda_over_scoped_formal_into_array_fill_rejected_spec_28_3_spec_28_14) {
    auto r = runAot(fboxHub(
        "  public method On(scoped handler: (double) -> ()) "
        "    var arr := new Array<(double) -> ()>(1, "
        "      lambda(t: double) => handler(t)); "
        "    _keep.f := arr[0]; "
        "  end On; "));
    expectScopedReject(r);
}

// 2 — the NAMED argument spelling (§22.5's binder: a named argument binds the
// parameter of that name — `fill`). RED on the base (admitted).
BEZEL_TEST(array_fill_capture_9353,
           named_fill_argument_lambda_over_scoped_formal_rejected_spec_28_3_spec_28_14) {
    auto r = runAot(fboxHub(
        "  public method On(scoped handler: (double) -> ()) "
        "    var arr := new Array<(double) -> ()>(size := 1, "
        "      fill := lambda(t: double) => handler(t)); "
        "    _keep.f := arr[0]; "
        "  end On; "));
    expectScopedReject(r);
}

// 3 — a carrier LOCAL as the fill argument ("a local carrying either").
// RED on the base (admitted).
BEZEL_TEST(array_fill_capture_9353,
           carrier_local_into_array_fill_rejected_spec_28_3_spec_28_14) {
    auto r = runAot(fboxHub(
        "  public method On(scoped handler: (double) -> ()) "
        "    var f := lambda(t: double) => handler(t); "
        "    var arr := new Array<(double) -> ()>(1, f); "
        "    _keep.f := arr[0]; "
        "  end On; "));
    expectScopedReject(r);
}

// 4 — the CALL-BOUNDED tier (§28.14's call-bounded default): "stored into any
// collection, a local one included". RED on the base (admitted;
// heap-use-after-free).
BEZEL_TEST(array_fill_capture_9353,
           lambda_over_call_bounded_formal_into_array_fill_rejected_spec_28_3_spec_28_14) {
    auto r = runAot(fboxHub(
        "  public method On(handler: (double) -> ()) "
        "    var arr := new Array<(double) -> ()>(1, "
        "      lambda(t: double) => handler(t)); "
        "    _keep.f := arr[0]; "
        "  end On; "));
    expectCallBoundedReject(r);
}
