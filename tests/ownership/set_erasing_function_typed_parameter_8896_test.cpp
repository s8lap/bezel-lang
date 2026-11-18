// Regression tests for issue #8896 — §28.3 closure bullet *Provenance*
// clause 3 and *Set-erasing positions* (v0.359).
//
// THE DEFECT.  `setErasingSourceSet` (src/ownership/Ownership_Checks.cpp),
// the trace behind `exprIsFunctionValueWithNonEmptyCaptureSet`, gave a
// parameter's INCOMING value no set at all (untraced), so a plain
// function-typed parameter read at a set-erasing position — `reg.Keep(q)`
// over `Keep(o: Object)` — was ADMITTED.  The callee stored the call-bounded
// closure past the call; the caller's argument temporary was released when
// the call returned, and the stored closure was then invoked after free
// (heap-use-after-free in `Reg2.RunInt`, whether or not the argument
// captured anything).
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
// Clause 6 reads the parameter "over every assignment that reaches the read",
// so an overwrite that dominates the read cuts the incoming value off (that
// program's verdict is the 8871 suite's `reassigned_function_typed_parameter`
// row and this lane's corpus candidates).
//
// ROWS.  Every row is RED on the base tree (r30-c1 @ b80340c9f): the program
// compiles there, and under `--sanitize=address` reports
// `heap-use-after-free` (MEASURED, 200 iterations; probe dir
// /var/tmp/bezel-scratch/r42-s93-8896/probes).
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
const char* kAtBareTypeParameter =
    "may not be passed where the parameter is declared at a bare type parameter";

void expectSetErasingReject(const JitResult& r, const char* position) {
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, kCarriesBorrow));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, position));
}

// The issue's harness, verbatim (issue #8896): `Reg2.Keep(o: Object)` stores
// the value and `RunInt()` invokes it after the call that passed it returned.
// `Keep<T>` gives the bare-type-parameter position.
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
           "public class Keep<T> "
           "  private _v: T? := null; "
           "  public constructor() end constructor; "
           "  public method Put(a: T) _v := a; end Put; "
           "end Keep; "
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

const char* kMkKeep =
    "  public static method Mk(reg: Reg2, q: () -> int) "
    "    reg.Keep(q); "
    "  end Mk; ";

}  // namespace

// REJECT — issue #8896's repro, verbatim (probe a1): the parameter `q` is
// read directly at the `Object` argument; the caller passes a closure over
// the scope-bound `r`.  RED on the base tree (compiled; heap-use-after-free).
BEZEL_TEST(set_erasing_function_typed_parameter_8896,
           issue_repro_parameter_at_object_argument_rejected_spec_28_3) {
    auto r = runAot(registryProgram(kMkKeep, "        Mk(reg, lambda() => r.V); "));
    expectSetErasingReject(r, kAtObject);
}

// REJECT — the issue's capture-FREE spelling (probe a2): the verdict is read
// off the callee's own body, where `q` "contributes itself" whatever the
// caller passes.  RED on the base tree (compiled; heap-use-after-free: the
// closure object itself is freed when `Mk` returns).
BEZEL_TEST(set_erasing_function_typed_parameter_8896,
           issue_repro_capture_free_argument_rejected_spec_28_3) {
    auto r = runAot(registryProgram(kMkKeep, "        Mk(reg, lambda() => 3); "));
    expectSetErasingReject(r, kAtObject);
}

// REJECT — a LAMBDA's own plain function-typed parameter (probe a5): a
// lambda body is "the enclosing body" of its parameter, and §28.14 seeds a
// lambda's plain function-typed parameters as call-bounded exactly as a
// method's.  RED on the base tree (compiled; heap-use-after-free).
BEZEL_TEST(set_erasing_function_typed_parameter_8896,
           lambda_own_parameter_at_object_argument_rejected_spec_28_3) {
    auto r = runAot(registryProgram(
        "  public static method Mk(reg: Reg2, r: Res) "
        "    var h := lambda(q: () -> int) => reg.Keep(q); "
        "    h(lambda() => r.V); "
        "  end Mk; ",
        "        Mk(reg, r); "));
    expectSetErasingReject(r, kAtObject);
}

// REJECT — an overwrite in ONE arm of an `if` does not cut the incoming value
// off (probe a6): the path through the empty `else` still carries `q`
// (clause 6, "over every assignment that reaches the read").  RED on the base
// tree (compiled; heap-use-after-free).
BEZEL_TEST(set_erasing_function_typed_parameter_8896,
           incoming_value_reaches_past_a_one_arm_overwrite_rejected_spec_28_3) {
    auto r = runAot(registryProgram(
        "  public static method Mk(reg: Reg2, q: () -> int, c: bool) "
        "    if c then q := lambda() => 5; end if; "
        "    reg.Keep(q); "
        "  end Mk; ",
        "        Mk(reg, lambda() => r.V, false); "));
    expectSetErasingReject(r, kAtObject);
}

// REJECT — the same parameter at a BARE TYPE PARAMETER substituted with a
// function type (probe a7): `k.Put(q)` at `k: Keep<() -> int>` over
// `Put(a: T)` is the bullet's own example of a set-erasing position ("a
// parameter ... whose declared, pre-substitution type ... is a bare type
// parameter, at an instantiation substituting a function type or `Object`").
// RED on the base tree (compiled).
BEZEL_TEST(set_erasing_function_typed_parameter_8896,
           parameter_at_bare_type_parameter_rejected_spec_28_3) {
    auto r = runAot(registryProgram(
        "  public static method Mk(reg: Reg2, q: () -> int) "
        "    var k := new Keep<() -> int>(); "
        "    k.Put(q); "
        "    reg.Keep(lambda() => 1); "
        "  end Mk; ",
        "        Mk(reg, lambda() => r.V); "));
    expectSetErasingReject(r, kAtBareTypeParameter);
}
