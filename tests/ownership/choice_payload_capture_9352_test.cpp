// Regression tests for issue #9352 — §28.3 *Function-typed holders* and
// §28.14 (v0.359): a capture-bearing closure over a `scoped` (or a plain,
// call-bounded) function-typed formal admitted as the PAYLOAD of a choice case
// whose payload field is declared at a function type, then bound out by a
// pattern and stored into a static field.
//
// Spec: §28.3 *Function-typed holders* (v0.359): "A **function-typed holder**
// is a field (a `scoped` field excepted), a record component, **a choice
// case's payload field**, an inline-array slot, … At every function-typed
// holder the closure bullet's sink rule holds: a store of a function value
// admits a capture-free value and rejects one whose source set is non-empty
// (a capture-bearing closure, **a plain function-typed parameter**, a local
// carrying either), judged at the site, whatever form the member is declared
// in. The stores are … **a payload argument at a case construction (§35.3, the
// declared-result form included)**". A payload bound out by a pattern carries
// the empty set (the same bullet, *Provenance* clause 5), which is why the
// later `_keep.f := g` is admitted — the laundering happens AT the payload.
// §28.14: the callee "may not … store it in any other object"; for the
// call-bounded tier: "a store into a function-typed record component,
// **choice payload** or inline-array slot".
//
// THE DEFECT. The holder rule reached a record component (§28.3's store list)
// and a field, but not a CHOICE PAYLOAD: the payload argument of
// `FC.A(lambda(t) => handler(t))` was admitted for both formal kinds, the
// closure was bound out by the pattern `A(g)` with the EMPTY source set, and
// `_keep.f := g` then stored it into a static field — the program died with an
// AddressSanitizer SEGV / heap-use-after-free when the stored closure ran
// (`#0 bz$P.Counter2$Hit #1 lam$1`).
//
// ROWS. One reject row per binding shape of the payload argument — the
// positional argument, the NAMED argument, the second of two payload fields,
// and a carrier LOCAL as the argument ("a local carrying either") — for the
// `scoped` formal, and the same positional shape for the call-bounded formal.
// The admitting twins (a capture-free payload, for both formal kinds) are in
// `corpus-candidates/`, not here: they are green on the base.
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
const char* kStoredInChoicePayload =
    "may not be stored in a function-typed choice payload";

// The §28.14 call-bounded tier's diagnostic.
const char* kCallBounded = "parameter `handler` is call-bounded";
const char* kStoredIntoChoicePayload =
    "may not be stored into a function-typed choice payload";

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
    "end FBox; "
    "public choice FC "
    "  A(f: (double) -> ()), "
    "  B; "
    "end FC; "
    "public choice FC2 "
    "  A(n: int, f: (double) -> ()), "
    "  B; "
    "end FC2; ";

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
std::string fcHub(const std::string& member) {
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
        bezel::test::diagnosticMessageContains(r, kStoredInChoicePayload));
}

// The §28.14 call-bounded tier's rejection.
void expectCallBoundedReject(const JitResult& r) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kCallBounded));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, kStoredIntoChoicePayload));
}

}  // namespace

// 1 — THE ISSUE'S REPRO, verbatim: the closure is the POSITIONAL payload
// argument of a case whose payload field is declared at a function type, and
// is then bound out by the pattern `A(g)` and stored into a static field.
// RED on the base (admitted; the stored closure SEGVs at run time).
BEZEL_TEST(choice_payload_capture_9352,
           lambda_over_scoped_formal_into_choice_payload_rejected_spec_28_3_spec_28_14) {
    auto r = runAot(fcHub(
        "  public method On(scoped handler: (double) -> ()) "
        "    var ch := FC.A(lambda(t: double) => handler(t)); "
        "    case ch of "
        "      A(g) => _keep.f := g, "
        "      _ => Console.Write(\"\") "
        "    end case; "
        "  end On; "));
    expectScopedReject(r);
}

// 2 — the NAMED argument spelling (§22.5's binder: a named argument binds the
// payload field of that name). RED on the base (admitted).
BEZEL_TEST(choice_payload_capture_9352,
           named_payload_argument_lambda_over_scoped_formal_rejected_spec_28_3_spec_28_14) {
    auto r = runAot(fcHub(
        "  public method On(scoped handler: (double) -> ()) "
        "    var ch := FC.A(f := lambda(t: double) => handler(t)); "
        "    case ch of "
        "      A(g) => _keep.f := g, "
        "      _ => Console.Write(\"\") "
        "    end case; "
        "  end On; "));
    expectScopedReject(r);
}

// 3 — the SECOND of two payload fields: the binder must bind the positional
// argument to the second field, not the first. RED on the base (admitted).
BEZEL_TEST(choice_payload_capture_9352,
           second_payload_field_lambda_over_scoped_formal_rejected_spec_28_3_spec_28_14) {
    auto r = runAot(fcHub(
        "  public method On(scoped handler: (double) -> ()) "
        "    var ch := FC2.A(1, lambda(t: double) => handler(t)); "
        "    case ch of "
        "      A(n, g) => _keep.f := g, "
        "      _ => Console.Write(\"\") "
        "    end case; "
        "  end On; "));
    expectScopedReject(r);
}

// 4 — a carrier LOCAL as the payload argument ("a local carrying either").
// RED on the base (admitted).
BEZEL_TEST(choice_payload_capture_9352,
           carrier_local_into_choice_payload_rejected_spec_28_3_spec_28_14) {
    auto r = runAot(fcHub(
        "  public method On(scoped handler: (double) -> ()) "
        "    var f := lambda(t: double) => handler(t); "
        "    var ch := FC.A(f); "
        "    case ch of "
        "      A(g) => _keep.f := g, "
        "      _ => Console.Write(\"\") "
        "    end case; "
        "  end On; "));
    expectScopedReject(r);
}

// 5 — the CALL-BOUNDED tier (§28.14's call-bounded default): a plain
// function-typed formal contributes itself to the closure's source set
// (§28.3 *Provenance* clause 3), so the same store is the same rejection.
// RED on the base (admitted; heap-use-after-free).
BEZEL_TEST(choice_payload_capture_9352,
           lambda_over_call_bounded_formal_into_choice_payload_rejected_spec_28_3_spec_28_14) {
    auto r = runAot(fcHub(
        "  public method On(handler: (double) -> ()) "
        "    var ch := FC.A(lambda(t: double) => handler(t)); "
        "    case ch of "
        "      A(g) => _keep.f := g, "
        "      _ => Console.Write(\"\") "
        "    end case; "
        "  end On; "));
    expectCallBoundedReject(r);
}
