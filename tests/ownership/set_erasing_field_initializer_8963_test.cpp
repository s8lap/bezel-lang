// Regression tests for issue #8963 — §28.3 closure bullet *Provenance*
// clause 3 and *Set-erasing positions* (v0.359), the FIELD-INITIALIZER twin
// of issue #8896's rows.
//
// THE DEFECT.  The §28.3 *Set-erasing positions* verdict is reached from ONE
// walker (`MethodOwnership::scopedPreScanExpr`, queued into
// `setErasingCheckQueue_` and judged by `reportSetErasingPosition`), and that
// walker runs only from `MethodOwnership::scopedPreScan`, which runs only
// from `MethodOwnership::run` — i.e. from a BLOCK method / constructor /
// operator body.  A lambda written in a FIELD INITIALIZER is not part of any
// block body, so:
//
//   (1) the `Object`-parameter argument inside that lambda's body was never
//       queued, so the position was never classified; and
//   (2) the lambda's OWN plain function-typed parameter was never seeded
//       into `callBoundedParams_` (the seeding lives in
//       `callBoundedScan` → `seedLambdaCallBoundedParams`, reached only from
//       `run`), so a read of that parameter read as UNTRACED instead of
//       NON-EMPTY — which is exactly what Provenance clause 3 says it is.
//
// The two halves together: the closure was stored where no rule could bound
// it (`Reg2.Keep(o: Object)`), and it was then invoked after its capture was
// freed.
//
// THE SPEC.  §28.3 (v0.359), closure bullet, *Provenance*, clause 3:
//
//   "A **plain function-typed parameter** of the enclosing body contributes
//    itself: its signature announces no set (K2.4), so the value is bounded
//    by the call, §28.14's call-bounded tier."
//
// and *Set-erasing positions*:
//
//   "At every set-erasing position a function value with a non-empty source
//    set is rejected, and a capture-free one is admitted"
//
// A LAMBDA's body is "the enclosing body" of its own parameters — the
// classification is the one a method body's lambda parameter already got
// (issue #8896's `lambda_own_parameter_at_object_argument` row), so a
// field-initialiser lambda's parameter gets exactly that provenance.
//
// ROWS.  Every row is RED on the base tree (r30-c1 @ eb5305418): the program
// compiles there, and under `--sanitize=address` reports
// `heap-use-after-free` in `Reg2.RunInt` (MEASURED, 200 iterations; probe dir
// /var/tmp/bezel-scratch/r43-m20-8963/probes).
//
// ORACLE.  `runAot`; the verdict is the specific set-erasing diagnostic at a
// real location — the same oracle the neighbouring #8896 rows use, because
// the expected outcome of the issue's program is a REJECTION, not a run.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

const char* kCarriesBorrow = "function value carries captured borrow(s)";
const char* kAtObject =
    "may not be passed where the parameter is declared Object";

void expectSetErasingReject(const JitResult& r) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kCarriesBorrow));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kAtObject));
}

// The issue's harness, verbatim (issue #8963): `Reg2.Keep(o: Object)` stores
// the value and `RunInt()` invokes it after the call that passed it returned.
// `Fl.H`'s FIELD INITIALIZER holds the lambda that stores it.
//
// `fieldDecl` is the field's declaration; `fieldUse` is how the field is read
// at the call site — `fl.H` for an instance field, `Fl.H` for a static one
// (a static member read through an instance is a §22.12 rejection of its
// own, which would make the row red for the wrong reason).
std::string registryProgram(const std::string& fieldDecl,
                            const std::string& fieldUse) {
    return "module M; "
           "import System; "
           "public class Res "
           "  public V: int; "
           "  public constructor() V := 42; end constructor; "
           "end Res; "
           "public class Reg2 "
           "  private _o: Object? := null; "
           "  public constructor() end constructor; "
           "  public method Keep(o: Object) _o := o; end Keep; "
           "  public method RunInt(): int "
           "    if _o is () -> int then return (_o as () -> int)!(); end if; "
           "    return 7; "
           "  end RunInt; "
           "end Reg2; "
           "public class Keep<T> "
           "  private _v: T? := null; "
           "  public constructor() end constructor; "
           "  public method Put(a: T) _v := a; end Put; "
           "end Keep; "
           "public record Box<T>(V: T); "
           "public class Fl "
           "  " +
           fieldDecl +
           "  public constructor() end constructor; "
           "end Fl; "
           "public class Program "
           "  public static method Mk(reg: Reg2, r: Res) "
           "    var fl := new Fl(); " +
           fieldUse +
           "(reg, lambda() => r.V); "
           "  end Mk; "
           "  public static method Main(): int "
           "    using var reg := new Reg2(); "
           "    for var i in 0 ..< 200 do "
           "      do "
           "        var r := new Res(); "
           "        Mk(reg, r); "
           "      end; "
           "      Console.WriteLine(reg.RunInt()); "
           "    end for; "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

}  // namespace

// REJECT — issue #8963's repro, verbatim: the lambda is an INSTANCE field's
// initializer, and its own plain function-typed parameter `q` is passed at
// `Reg2.Keep(o: Object)`'s `Object` parameter — a set-erasing position.
// Provenance clause 3 gives `q` a NON-EMPTY source set (it contributes
// itself), so the position rejects it.  RED on the base tree (compiled;
// heap-use-after-free in `Reg2.RunInt`).
BEZEL_TEST(set_erasing_field_initializer_8963,
           issue_repro_field_initializer_lambda_parameter_at_object_argument_rejected_spec_28_3) {
    auto r = runAot(registryProgram(
        "public H: (Reg2, () -> int) -> () := "
        "lambda(rg: Reg2, q: () -> int) => rg.Keep(q); ",
        "fl.H"));
    expectSetErasingReject(r);
}

// REJECT — the STATIC field initializer twin: the same lambda in a `static
// readonly` field's initializer.  A static field's initializer is a
// declaration-site expression like an instance field's, and the stored
// closure outlives the frame that formed it the same way.  RED on the base
// tree (compiled; heap-use-after-free).
BEZEL_TEST(set_erasing_field_initializer_8963,
           static_field_initializer_lambda_parameter_at_object_argument_rejected_spec_28_3) {
    auto r = runAot(registryProgram(
        "public static readonly H: (Reg2, () -> int) -> () := "
        "lambda(rg: Reg2, q: () -> int) => rg.Keep(q); ",
        "Fl.H"));
    expectSetErasingReject(r);
}

// REJECT — the NESTED lambda twin: the parameter `x` belongs to a lambda
// nested INSIDE the field initializer's lambda, and it is `x` — not the outer
// lambda's own parameter — that is read at the `Object` parameter.  Each
// lambda occurrence mints its own parameter symbols, so the nested one is
// seeded (and classified) on its own.  RED on the base tree (compiled).
BEZEL_TEST(set_erasing_field_initializer_8963,
           nested_lambda_parameter_at_object_argument_rejected_spec_28_3) {
    auto r = runAot(registryProgram(
        "public H: (Reg2, () -> int) -> () := "
        "lambda(rg: Reg2, q: () -> int) => "
        "rg.Keep(lambda(x: () -> int) => rg.Keep(x)); ",
        "fl.H"));
    expectSetErasingReject(r);
}
