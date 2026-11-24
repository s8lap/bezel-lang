// Regression tests for issue #9034 — §28.3 closure bullet *Provenance*
// clause 3 and *Set-erasing positions* (v0.359), the ACCESSOR-BLOCK-BODY
// twin of issue #8896's rows.
//
// THE DEFECT.  The §28.3 *Set-erasing positions* verdict is reached from ONE
// walker (`MethodOwnership::scopedPreScanExpr`, queued into
// `setErasingCheckQueue_` and judged by `reportSetErasingPosition`), and that
// walker runs only from `MethodOwnership::run` — i.e. from a BLOCK method /
// constructor / operator body, a `set(owned)` accessor body
// (`runOwnedSetterBody`), and a block getter body (`runGetterBody`, and then
// only when the body is PROVED CONFINED, and only in the getter-scope-only
// mode whose diagnostics are deliberately suppressed).  A PLAIN setter's block
// body (`set(v) => do … end;`) runs no ownership pass at all (#5252 scoped the
// `set(owned)` fix to that spelling), and an UNCONFINED block getter body runs
// none either — so at those positions the rule was never reached.
//
// And a LAMBDA's own plain function-typed parameters are seeded as
// call-bounded only there too (`callBoundedScan` → `seedLambdaCallBoundedParams`),
// so a lambda written in an accessor block body had its own parameter read as
// UNTRACED rather than NON-EMPTY — exactly what Provenance clause 3 says it
// is.  A function value with a non-empty source set was therefore stored
// where no rule could bound it (`Reg2.Keep(o: Object)`), and the closure it
// stores was then invoked after the frame that formed it was gone (#8963's
// use-after-free shape).
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
// (issue #8896's `lambda_own_parameter_at_object_argument` row), so an
// accessor block body's lambda parameter gets exactly that provenance.
//
// ROWS.  Every row is RED on the base tree (r30-c1 @ e598654aa): the program
// compiles there (MEASURED — the issue's program, verbatim; and under
// `--sanitize=address` the shape that stores the closure in a field reports
// `heap-use-after-free` once the accessor body has returned, 200 iterations;
// probe dir /var/tmp/bezel-scratch/r43-m24-9034/probes).
//
// ORACLE.  `runAot`; the verdict is the specific set-erasing diagnostic at a
// real location — the same oracle the neighbouring #8896 / #8963 rows use,
// because the expected outcome of the issue's program is a REJECTION, not a
// run.

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

// The issue's harness, shaped like #8896's / #8963's: `Reg2.Keep(o: Object)`
// STORES the value, and `Reg2.RunInt()` INVOKES it after the call that passed
// it returned.  `accessor` is the accessor's body text; `exercise` is the
// statement that runs it.
std::string accessorProgram(const std::string& accessor,
                            const std::string& exercise) {
    return "module M; "
           "import System; "
           "public class Reg2 "
           "  private _o: Object? := null; "
           "  public constructor() end constructor; "
           "  public method Keep(o: Object) _o := o; end Keep; "
           "  public method RunInt(): int "
           "    if _o is () -> int then return (_o as () -> int)!(); end if; "
           "    return 7; "
           "  end RunInt; "
           "end Reg2; "
           "public class Fl "
           "  public constructor() end constructor; "
           "  public property P: int "
           "    " + accessor + " "
           "  end; "
           "end Fl; "
           "public class Program "
           "  public static method Main(): int "
           "    using var reg := new Reg2(); "
           "    var fl := new Fl(); "
           "    for var i in 0 ..< 200 do "
           "      do " + exercise + " end; "
           "      Console.WriteLine(reg.RunInt()); "
           "    end for; "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

// The issue's repro, verbatim: a plain setter's BLOCK body.  Its lambda's own
// plain function-typed parameter `x` is read at `Reg2.Keep(o: Object)`'s
// `Object` parameter — a set-erasing position — and the closure that lambda
// is also passed at the outer `Reg2.Keep(o: Object)`, where it carries the
// scope-bound `reg` it captures.
const char* kPlainSetterBlockBody =
    "set(v) => do var reg := new Reg2(); "
    "reg.Keep(lambda(x: () -> int) => reg.Keep(x)); end;";

// The clause-3 row's accessor: the closure itself captures NOTHING (its two
// parameters are its own), so the closure is capture-free and the position
// that rejects is the INNER one — the lambda's OWN plain function-typed
// parameter `q`, read at `Reg2.Keep(o: Object)`.  Provenance clause 3 gives
// `q` a NON-EMPTY source set (it contributes itself), so the position
// rejects it.
const char* kClause3Accessor =
    "set(v) => do var reg := new Reg2(); "
    "var h := lambda(rg: Reg2, q: () -> int) => rg.Keep(q); "
    "h(reg, lambda() => 1); end;";

// The block getter's body: the same shape as the setter's, in the OTHER
// accessor spelling.
const char* kBlockGetterBody =
    "get => do var reg := new Reg2(); "
    "reg.Keep(lambda(x: () -> int) => reg.Keep(x)); return 0; end;";

}  // namespace

// REJECT — issue #9034's repro, verbatim (probe p01): a PLAIN setter's block
// body.  No ownership pass reaches that body, so the §28.3 *Set-erasing
// positions* rule was never applied there and the closure was stored at an
// `Object` parameter with a non-empty source set.  RED on the base tree
// (compiled).
BEZEL_TEST(set_erasing_accessor_block_body_9034,
           issue_repro_plain_setter_block_body_rejected_spec_28_3) {
    auto r = runAot(accessorProgram(kPlainSetterBlockBody, "fl.P := 1;"));
    expectSetErasingReject(r);
}

// REJECT — the CLAUSE-3 row (probe p05): the closure is capture-free, so the
// only non-empty source set at the position is the lambda's OWN plain
// function-typed parameter `q` — Provenance clause 3.  The byte-identical
// lambda in a METHOD body is rejected (issue #8896's row), so the accessor
// body must reject it too.  RED on the base tree (compiled).
BEZEL_TEST(set_erasing_accessor_block_body_9034,
           lambda_own_parameter_at_object_argument_in_accessor_body_rejected_spec_28_3) {
    auto r = runAot(accessorProgram(kClause3Accessor, "fl.P := 1;"));
    expectSetErasingReject(r);
}

// REJECT — the BLOCK GETTER twin (probe p03): the same shape in the other
// accessor spelling.  A block getter's body runs the ownership pass only when
// it is PROVED CONFINED, and then in the getter-scope-only mode whose
// diagnostics are suppressed — so the rule was never reached there either.
// RED on the base tree (compiled).
BEZEL_TEST(set_erasing_accessor_block_body_9034,
           block_getter_body_rejected_spec_28_3) {
    auto r = runAot(accessorProgram(kBlockGetterBody, "Console.WriteLine(fl.P);"));
    expectSetErasingReject(r);
}
