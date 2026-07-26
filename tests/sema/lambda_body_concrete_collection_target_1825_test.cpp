// Sema fix #1825 / §15.4: a collection literal as a LAMBDA'S expression body
// against a CONCRETE admissible return type (e.g. `(int) -> Array<int>`) is now
// materialised — per §15.4 the literal HAS a target (the lambda's declared
// return type is the closed-list concrete `Array<int>`, the same target type
// that the call-arg / local-decl paths already use), so codegen builds the
// Array from the literal and the program compiles + runs + returns the correct
// element count. Before the fix sema saw the literal in an expression-bodied
// lambda with no type (the element type U of a hypothetical IEnumerable<U>
// selector was inferred FROM the body, a circular case), lowered to a null /
// empty result, and the program either miscompiled (exit 0 / count 0,
// #1693's pre-fix shape) or rejected with §15.4 (post-#1693). This test
// pins the CONCRETE-target acceptance: the lambda's return is a §15.4
// admissible type, so the literal materialises.
//
// Sibling of #1693 / selectmany_literal_selector_1693_test — that one pins
// the §15.4 reject for the selector / no-concrete-target case; this one
// pins the accept for the typed-local / function-typed-call-arg case.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAot — AOT + native exec oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// AOT-compile+run; return the `main` exit code (or -999 if it did not run).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ---------------------------------------------------------------------------
// ACCEPT — concrete §15.4 return target materialises the literal.
// ---------------------------------------------------------------------------

// §15.4 (#1825): the exact issue-body repro. A typed local of a
// function-typed `(int) -> Array<int>` with a literal-bodied lambda compiles,
// runs, and returns the count of elements actually materialised (2 = x and
// x+10 for x=5). Without the fix this rejected with the §15.4
// collection-literal-target diagnostic. With the fix the literal is
// materialised into an Array<int> by codegen, exactly like the
// `var arr: Array<int> := [x, x+10]` local-init path (#1866).
BEZEL_TEST(lambda_body_concrete_collection_target_1825,
           typed_local_ienumerable_array_target_accepts_15_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var f: (int) -> Array<int> := lambda(x) => [x, x + 10]; "
        "  var r := f(5); "
        "  return r.Count(); "                        // 2 = [5, 15]
        "end Main; end C;"), 2);
}

// §15.4 (#1825): the lambda is bound to a FUNCTION-TYPED parameter of a
// method — the call-argument case. Without the fix this also rejected (no
// concrete target on the selector), now it accepts. Apply invokes the
// lambda, the result is an Array<int> = [5, 15], the loop sums to 20.
// The lambda is passed DIRECTLY to Apply (not invoked first) so the
// call-arg path (ExprTypecheck_Exprs.cpp L4726+) threads
// pendingLambdaTargetReturn_ correctly.
BEZEL_TEST(lambda_body_concrete_collection_target_1825,
           call_arg_function_typed_array_target_accepts_15_4) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Apply(f: (int) -> Array<int>): int "
        "    var arr := f(5); "
        "    var s := 0; "
        "    for var v in arr do s := s + v; end; "
        "    Console.WriteLine(s.ToString()); "
        "    return s; "
        "  end Apply; "
        "  public static method Main(): int "
        "    return C.Apply(lambda(x: int) => [x, x + 10]); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // 5+15 = 20 (the value the program prints). A pre-#1825 compile FAILS
    // (no materialisation), so r.ran would be false and the stdout pin
    // would simply not find "20" — both expected.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("20") != std::string::npos);
}

// §15.4 (#1825): a CONCRETE non-array §15.4 target — `OwningArray<MyClass>` —
// also materialises (the closed list includes `OwningArray`). The element
// type must be a CLASS per stdlib §4.2 / #1327 (an owning collection only
// owns class/interface-typed elements — a primitive has no disposal
// obligation and is rejected as a §14.4 mismatch). Pins the
// `OwningArray` half of the §15.4 closed list specifically.
// // per §15.4, stdlib §4.2 / #1327
BEZEL_TEST(lambda_body_concrete_collection_target_1825,
           typed_local_owning_array_target_accepts_15_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell public V: int := 0; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Cell; "
        "class C public static method Main(): int "
        "  var f: (int) -> OwningArray<Cell> := lambda(x) => "
        "    [new Cell(x), new Cell(x + 10)]; "
        "  var r := f(5); "
        "  return r.Count(); "                        // 2
        "end Main; end C;"), 2);
}

// ---------------------------------------------------------------------------
// REJECT (no regression) — interface target still rejects per #1693/#1174.
// ---------------------------------------------------------------------------

// §15.4 (#1825 / #1693): the selector / no-concrete-target case still
// REJECTS. The lambda's return type is inferred from the LINQ selector
// signature (`IEnumerable<U>`), an interface not on the §15.4 closed list —
// the body has no materialisable target, so the §15.4 diagnostic fires.
// This is the regression guard for #1693's fix; the new accept path must
// not have silently turned the reject into an empty-materialised accept.
BEZEL_TEST(lambda_body_concrete_collection_target_1825,
           selector_interface_target_still_rejected_15_4_1693) {
    auto r = runAot(
        "module M; "
        "import System.Collections.Linq; "
        "class C public static method Main(): int "
        "  using var xs := new Array<int>(); "
        "  xs.Add(1); xs.Add(2); "
        "  var n := 0; "
        "  for var y in xs.SelectMany(lambda(x) => [x, x + 10]) do "
        "    n := n + 1; "
        "  end; "
        "  return n; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
    // The §15.4 diagnostic must cite the section (regression guard for
    // #1693's diagnostic message).
    bool found154 = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("\xC2\xA7""15.4") != std::string::npos) {
            found154 = true; break;
        }
    BEZEL_EXPECT_TRUE(found154);
}
