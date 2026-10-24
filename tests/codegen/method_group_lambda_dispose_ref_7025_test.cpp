// fix #7025 (follow-up, §27.6 / §28.2, v0.343): a LAMBDA whose body is a
// method-group reference to a class's own compiler-generated `Dispose`.
//
// THE DEFECT. §27.6: "the member resolved on the value exactly as the call
// `v.Scaled()` is" — the bare reference `d.Dispose` is a method reference of
// type `() -> ()` (§28.2: `Dispose()` takes no parameter and returns
// nothing). A lambda whose body is that reference returns a FUNCTION VALUE.
//
// The generated `Dispose` has NO MethodDecl and NO MethodSymbol (§28.2: "the
// name is the compiler's"), so sema binds NO type to the reference and the
// lambda's BODY type is null. The expr-body return type was then the `i32`
// fallback while the body lowered to a FUNCTION VALUE (a closure cell), and
// the emitted `ret` did not match the function's return type:
//
//   Function return type does not match operand type of return inst!
//   ret ptr %5
//
// — the IR verifier rejected the module and the compile failed with "IR
// verification failed (malformed IR emitted)". The program the base compiled
// and ran (§28.2's use-after-dispose warning and all) became a compile
// failure.
//
// THE RULE. §12.7: a function value's own type is the type it was formed at;
// the formed type of a method reference is "the callee's own signature read as
// a function type" (§27.6). The lambda's body is a function value, so the
// lambda's return type is that function value's representation — the same one
// the ordinary §27.6 reference (`lambda() => d.Ping`) already got, because
// sema types THAT one.
//
// THE ROWS BELOW.
//
//   * the class-`Dispose` reference as a lambda body: the program COMPILES and
//     RUNS (pre-fix: "IR verification failed (malformed IR emitted)", so
//     `r.ran` is false);
//   * the ordinary §27.6 reference as a lambda body: the CONTROL — it already
//     behaved this way, and the class-`Dispose` twin must behave the same.
//
// The rows are NOT pinned with `runAotChecked`: the lambda's RESULT — the
// method group it returns — is DISCARDED by the statement that calls it, and
// a discarded function value is not released by the compiler (a pre-existing
// gap, shared with the ordinary §27.6 reference — measured: the same 72
// bytes, the same allocation site, for `lambda() => d.Ping`). The row that
// pins the §28.2 use-after-dispose warning is in tests/ownership/.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

constexpr char kClassDisposeLambda[] =
    "module M; "
    "class D "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"disposed\"); end destructor; "
    "end D; "
    "class C public static method Main(): int "
    "  var d := new D(); "
    "  var f := lambda() => d.Dispose; "
    "  d.Dispose(); "
    "  f(); "
    "  Console.WriteLine(\"after\"); "
    "  return 0; "
    "end Main; end C;";

constexpr char kOrdinaryRefLambda[] =
    "module M; "
    "class D "
    "  public constructor() end constructor; "
    "  public method Ping(): int return 42; end Ping; "
    "end D; "
    "class C public static method Main(): int "
    "  var d := new D(); "
    "  var f := lambda() => d.Ping; "
    "  f(); "
    "  Console.WriteLine(\"after\"); "
    "  return 0; "
    "end Main; end C;";

}  // namespace

// The class's own `Dispose` as a lambda body. The lambda returns a method
// group; the program compiles, runs, and prints what the base printed.
//
// PRE-FIX: the compile failed with "IR verification failed (malformed IR
// emitted)" — the IR verifier rejected the module.
BEZEL_TEST(method_group_lambda_dispose_ref_7025,
           spec_27_6_lambda_body_returning_the_generated_dispose_compiles_and_runs) {
    auto r = runAot(kClassDisposeLambda);
    // `r.ran` is false when the driver rejects the program — the pre-fix
    // failure mode, with the verifier's message in `r.diagnostics`.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // The explicit `Dispose()` runs the destructor once ("disposed"); the
    // lambda's invocation FORMS the method group (and returns it) without
    // calling it, so nothing else prints.
    BEZEL_EXPECT_EQ(r.stdoutText, "disposed\nafter\n");
}

// The CONTROL: the ORDINARY §27.6 method reference as a lambda body —
// `lambda() => d.Ping`. Sema types that one, so it always compiled and ran;
// the class-`Dispose` twin must behave exactly like it.
BEZEL_TEST(method_group_lambda_dispose_ref_7025,
           spec_27_6_lambda_body_returning_an_ordinary_method_reference_behaves_the_same) {
    auto r = runAot(kOrdinaryRefLambda);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "after\n");
}
