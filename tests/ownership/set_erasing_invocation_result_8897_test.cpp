// Regression tests for issue #8897 — §28.3 closure bullet *Provenance*
// clause 4 and *Set-erasing positions* (v0.359).
//
// THE DEFECT.  `setErasingSourceSet` (src/ownership/Ownership_Checks.cpp),
// the trace behind `exprIsFunctionValueWithNonEmptyCaptureSet`, had no arm
// for a CALL expression: the result of invoking a function value read as
// untraced, so a closure RETURNED by a capturing lambda — `reg.Keep(mk())`
// over `mk := lambda() => lambda() => r.V` — was ADMITTED at an `Object`
// argument.  The stored closure outlived `r` and read it after free
// (heap-use-after-free in `lam$1`, called from `Reg2.RunInt`).
//
// THE SPEC.  §28.3 (v0.359), closure bullet, *Provenance*, clause 4:
//
//   "**Invoking** a function value yields a value carrying the invoked
//    value's set (a lambda's body can return a closure only over what the
//    lambda itself captures, its own locals being scope-bound; a method's
//    function-typed return is capture-free under the sink rule, so a method
//    call's result carries the empty set)"
//
// and *Set-erasing positions*:
//
//   "At every set-erasing position a function value with a non-empty source
//    set is rejected, and a capture-free one is admitted"
//
// ROWS.  Every row is RED on the base tree (r30-c1 @ b80340c9f): the program
// compiles there (MEASURED; the issue's two spellings also report
// `heap-use-after-free` under `--sanitize=address`, 200 iterations; probe dir
// /var/tmp/bezel-scratch/r42-s93-8896/probes).  The admitted controls (a
// capture-free maker, an `int` result, a method call's result) are green on
// both trees and live in the lane's corpus candidates.
//
// ORACLE.  `runAot`; the verdict is the specific set-erasing diagnostic at a
// real location.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

const char* kCarriesBorrow = "function value carries captured borrow(s)";
const char* kAtObject = "may not be passed where the parameter is declared Object";

void expectSetErasingReject(const JitResult& r) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kCarriesBorrow));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kAtObject));
}

// The issue's harness, verbatim (issue #8897): `Reg2.Keep(o: Object)` stores
// the value and `RunInt()` invokes it after `r` was disposed at the end of
// the inner `do` block.
std::string registryProgram(const std::string& mk, const std::string& call) {
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
           "  public method Swap(o: Object): () -> int _o := o; return lambda() => 0; end Swap; "
           "  public method RunInt(): int => (_o as () -> int)!(); "
           "end Reg2; "
           "public class Program " +
           mk +
           "  public static method Main(): int "
           "    using var reg := new Reg2(); "
           "    for var i in 0 ..< 200 do "
           "      do "
           "        var r := new Res(); " +
           call +
           "      end; "
           "      Console.WriteLine(reg.RunInt()); "
           "    end for; "
           "    return 0; "
           "  end Main; "
           "end Program; ";
}

std::string mk2(const std::string& body) {
    return "  public static method Mk(reg: Reg2, r: Res) " + body + "  end Mk; ";
}

const char* kCall2 = "        Mk(reg, r); ";

}  // namespace

// REJECT — issue #8897's repro, verbatim (probe b1): the invocation result is
// read directly at the `Object` argument.  RED on the base tree (compiled;
// heap-use-after-free).
BEZEL_TEST(set_erasing_invocation_result_8897,
           issue_repro_invocation_result_at_object_argument_rejected_spec_28_3) {
    auto r = runAot(registryProgram(
        mk2("    var mk := lambda() => lambda() => r.V; "
            "    reg.Keep(mk()); "),
        kCall2));
    expectSetErasingReject(r);
}

// REJECT — the issue's LOCAL spelling (probe b2): `f`'s one definition is the
// invocation, which clause 6 traces to clause 4.  RED on the base tree
// (compiled; heap-use-after-free).
BEZEL_TEST(set_erasing_invocation_result_8897,
           issue_repro_local_bound_to_invocation_result_rejected_spec_28_3) {
    auto r = runAot(registryProgram(
        mk2("    var mk := lambda() => lambda() => r.V; "
            "    var f := mk(); "
            "    reg.Keep(f); "),
        kCall2));
    expectSetErasingReject(r);
}

// REJECT — invoking a plain function-typed PARAMETER (probe b3): clause 3
// gives `q` a non-empty set ("contributes itself") and clause 4 carries it to
// the result.  RED on the base tree (compiled; heap-use-after-free).
BEZEL_TEST(set_erasing_invocation_result_8897,
           invoked_function_typed_parameter_result_rejected_spec_28_3) {
    auto r = runAot(registryProgram(
        "  public static method Mk(reg: Reg2, q: () -> () -> int) "
        "    reg.Keep(q()); "
        "  end Mk; ",
        "        Mk(reg, lambda() => lambda() => r.V); "));
    expectSetErasingReject(r);
}

// REJECT — the invoked value under a postfix `!` (probe b4): the invoked
// value is the operand's.  RED on the base tree (compiled;
// heap-use-after-free).
BEZEL_TEST(set_erasing_invocation_result_8897,
           invocation_through_postfix_bang_rejected_spec_28_3) {
    auto r = runAot(registryProgram(
        mk2("    var mk: (() -> () -> int)? := lambda() => lambda() => r.V; "
            "    reg.Keep(mk!()); "),
        kCall2));
    expectSetErasingReject(r);
}
