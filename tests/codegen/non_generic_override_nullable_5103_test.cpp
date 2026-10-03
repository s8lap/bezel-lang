// Regression tests for issue #5103 — a NON-VIRTUAL direct call to a method with
// an explicitly-typed nullable-value formal (`Do(x: double?)`) receiving an
// `int?` argument, and the argument reaching the call UNLIFTED as `{i8, i32}`
// against the `{i8, double}` formal.
//
// COVERAGE GAP THIS FILE CLOSES. fix #4967 (e2aa55d0) put
// `widenNullableArgPayload` in `buildOrderedArgs`, and all FIVE of its own rows
// in `nullable_arg_lift_widen_4967_test.cpp` are `public static method` calls.
// A static call and an INSTANCE-method direct call reach that lift by different
// routes — the instance form goes through `emitMethodCall`'s direct-call path
// (`CreateCall(callee, args)`) with the receiver bound, and nothing pinned that
// route. #5103 is that route, and its fix was never landed: the abandoned
// 26d052e9 re-land was measured by the 5103-am lane to be dead code on top of
// #4967. This file is the test-only pin #5103 asked for.
//
// WHY A PLAIN NON-VIRTUAL METHOD, AND NOT AN `override`. MEASURED on this
// tree: `virtual`, `override` and `sealed override` all emit VTABLE dispatch
// (`call *0x30(%rax)`) for `d.Do(...)` on a `Derived`-typed receiver, because
// the receiver's dynamic type is unknown at the call site; only a member with
// no dispatch modifier emits the direct `call bz$M.D$Do`. So the direct-call
// route #5103 is about is reachable ONLY through a non-virtual method, and a
// `Base`/`Derived` pair cannot express it: declaring `override` over a
// non-virtual base member is rejected (§12.1 — "`Do` is declared `override` but
// the base member it would override is not `virtual`, `abstract`, or
// `override`"). The plain-method form is therefore the shape, and the class
// name / `Do` spelling follow the issue's repro so the pin is greppable.
//
// FAIL-WITHOUT (MEASURED). Disabling the `widenNullableArgPayload` lambda in
// `buildOrderedArgs` (src/codegen/LlvmBackend_Assign.cpp) and rebuilding
// bezelc turns row 1's program into a hard compile failure:
//   { i8, double } %19 = call { i8, double } @"bz$M.D$Do"(ptr %17, { i8, i32 } %18)
//   Call parameter type does not match function signature!
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
// The same signature mismatch #4967 reports, on the direct-call route.
//
// spec §5.1 "Implicit conversions" (int → double, float → double) and
// "Nullable conversions" (the rules compose through at most one `?` on each
// side), §22.6 stage 3 (an applicable argument's implicit conversion is
// applied), §12.1 (override requires a virtual/abstract/override base member),
// v0.313.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §5.1 / §22.6 (v0.313), fix #4967 on the NON-VIRTUAL DIRECT-CALL route of
// #5103 — the issue's `Do(x: double?)` shape with an `int?` argument, present
// and null. The null argument pins that the tag survives the lift (a lift that
// dropped or inverted the `hasValue` byte would print -1 here).
BEZEL_TEST(non_generic_override_nullable_5103,
           direct_call_int_arg_at_double_nullable_formal) {
    expectPrints(
        "module M; "
        "class Derived "
        "  public method Do(x: double?): double? return x ?? -1.0; end Do; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(); "
        "    var q: int? := 5; var n: int? := null; "
        "    var r: double? := d.Do(q); var s: double? := d.Do(n); "
        "    Console.WriteLine($\"{r ?? -99.0}/{s ?? -99.0}\"); return 0; "
        "  end Main; "
        "end C;",
        "5/-1\n");
}

// per §5.1 (v0.313), fix #4967 on #5103's direct-call route — the OTHER §5.1
// widening direction into the same `double?` formal: `float?` at `double?`,
// an FPExt of the payload. Distinct code path inside the lift from row 1's
// sitofp, and a float payload that failed to widen would print 1.0, 2.0, 3.0
// back at half precision rather than the exact doubles.
BEZEL_TEST(non_generic_override_nullable_5103,
           direct_call_float_arg_at_double_nullable_formal) {
    expectPrints(
        "module M; "
        "class Derived "
        "  public method Do(x: double?): double? return x ?? -1.0; end Do; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(); "
        "    var f: float? := 0.1f; var n: float? := null; "
        "    Console.WriteLine($\"{d.Do(f) ?? -99.0}/{d.Do(n) ?? -99.0}\"); "
        "    return 0; "
        "  end Main; "
        "end C;",
        "0.10000000149011612/-1\n");
}

// per §5.1 (v0.313), fix #4967 on #5103's direct-call route — 50 direct calls
// in a loop alternating a present and a null `int?` argument. Not a one-shot:
// the loop defeats an oracle that only catches a single malformed call, and
// BOTH the running sum and the count of non-negative results pin every lifted
// payload and its hasValue tag (a lift that widened but dropped or inverted a
// tag moves one of the two numbers).
BEZEL_TEST(non_generic_override_nullable_5103,
           direct_call_loop_50_iterations_alternating) {
    expectPrints(
        "module M; "
        "class Derived "
        "  public method Do(x: double?): double return x ?? -1.0; end Do; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(); "
        "    var sum: double := 0.0; var present: int := 0; "
        "    for var i in 0..<50 do "
        "      var q: int? := null; "
        "      if i mod 2 = 0 then q := i; end; "
        "      var v: double := d.Do(q); "
        "      sum := sum + v; "
        "      if v >= 0.0 then present := present + 1; end; "
        "    end; "
        "    Console.WriteLine($\"{sum}/{present}\"); return 0; "
        "  end Main; "
        "end C;",
        "575/25\n");
}
