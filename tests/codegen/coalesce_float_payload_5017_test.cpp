// Regression tests for issue #5017 (site B) — §7.7 typing ("for a left
// operand of type `T?`, the right operand must be implicitly convertible
// (§5.1) to `T` …; the result type is `T`") with §5.1's int → float/double
// and float → double widenings, v0.307.
//
// `lowerCoalesce` (LlvmBackend_Exprs2.cpp) coerced a non-nullable RHS only
// to an INTEGER payload of another width or to a `decimal` payload. For a
// binary-float payload (`double? ?? -1`) the `i32` RHS reached the result
// PHI at its own type and the LLVM verifier aborted the compile —
// MEASURED on origin/main e3b0bdf0: "PHI node operands are not the same
// type as the result! %27 = phi double [ %nv.val, %cat.done ], [ -1,
// %coal.rhs ]" followed by "codegen error: IR verification failed". Each
// row therefore asserts the specific IR-verification diagnostic is absent
// as well as the exact output.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §7.7 / §5.1 (v0.307), fix #5017 — `double? ?? -1`, present and null.
BEZEL_TEST(coalesce_float_payload_5017, double_nullable_coalesce_int_literal) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var x: double? := 3.14; "
        "    Console.WriteLine($\"res={x ?? -1}\"); "
        "    var n: double? := null; "
        "    Console.WriteLine($\"res={n ?? -1}\"); "
        "  end Main; "
        "end C;",
        "res=3.14\nres=-1\n");
}

// per §5.1 / §7.7 (v0.307), fix #5017 — the issue's IR-crash repro verbatim
// (a lifted reassignment, then `?? -1`; needs site A too for the value).
BEZEL_TEST(coalesce_float_payload_5017, issue_repro_lift_then_coalesce) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var ki: int? := 4; "
        "    var x: double? := null; "
        "    x := ki; "
        "    Console.WriteLine($\"lift {x ?? -1}\"); "
        "  end Main; "
        "end C;",
        "lift 4\n");
}

// per §5.1 (v0.307), fix #5017 — the other float-payload widenings: a
// `float` RHS into a `double` payload (FPExt), a `uint` RHS (unsigned, so
// 4000000000 not a negative value), and an `int` RHS into a `float` payload.
BEZEL_TEST(coalesce_float_payload_5017, float_rhs_unsigned_rhs_and_float_payload) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var n: double? := null; "
        "    var g: float := 2.5f; "
        "    Console.WriteLine($\"a {n ?? g}\"); "
        "    var u: uint := 4000000000; "
        "    var w: double := n ?? u; "
        "    if w > 3000000000.0 then Console.WriteLine(\"b big\"); "
        "    else Console.WriteLine(\"b small\"); end if; "
        "    var fz: float? := null; "
        "    var k: int := 7; "
        "    Console.WriteLine($\"c {fz ?? k}\"); "
        "  end Main; "
        "end C;",
        "a 2.5\nb big\nc 7\n");
}
