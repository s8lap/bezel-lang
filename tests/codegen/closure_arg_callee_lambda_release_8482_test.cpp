// Codegen — issue #8482: a closure local passed as an ARGUMENT was never
// released when the CALLEE's body contained any lambda expression, even one
// that never names the parameter.
//
// THE RULE. §28.14 (v0.359): a function-typed parameter declared without
// `scoped` is call-bounded — "a plain parameter is **call-bounded** (the
// callback is scoped to the call)". §28.9 (v0.359, "Runtime-managed types"):
// a function value's lifetime "is managed by the runtime without user
// involvement", so the caller's closure local is reclaimed once nothing holds
// it. §27.5 (v0.359): "A lambda may reference variables from its enclosing
// scope" — a lambda captures what it references, so one that never names the
// parameter cannot hold it. (§28.9 leaves the mechanism to the
// implementation; the citation is for the observable guarantee: no leak.)
//
// ROOT CAUSE. The caller's escape walk (`ClosureEscapeWalker`) admits a
// closure local passed to a plain function-typed, `Object`, or method-own
// type-parameter formal only when the callee body uses that formal by
// invocation (or a null / type test) alone:
// `closureArgBindsMethodOwnTypeParamBorrow` ->
// `calleeBodyUsesFormalOnlyByInvocation[OrInspection]`, whose walk
// (`FormalInvocationOnlyWalk`, LlvmBackend_Calls2.cpp) failed closed on EVERY
// LambdaExpr. A callee with any lambda therefore made the argument an escape,
// and the caller's scope-end `bz_closure_release` was never armed. MEASURED
// on 6d709c00f: `main` of the issue's repro has no `bz_closure_release` of
// `g`; the lambda-free control has two (normal and exception exit).
//
// THE FIX. The walk descends into a lambda body; inside one, every occurrence
// of the formal (an invocation and a null / type test included) is a capture
// and fails, and a lambda parameter spelled like the formal fails closed as a
// shadowing local does. A callee lambda that captures the formal therefore
// keeps today's verdict (escape -> leak, the safe direction); measured, not
// pinned here, because those programs behave the same on both arms (they are
// in the lane's corpus-candidates).
//
// Every row runs under `runAotChecked` (generated-code AddressSanitizer +
// LeakSanitizer) with the full stdout compared; every row after the first
// declares the closure local inside a 100-iteration loop, so the leak it pins
// is 100 allocations.
//
// MEASURED (`bezelc --sanitize=address --link=system`, detect_leaks=1):
//   row                                                 6d709c00f            with the fix
//   issue_repro_verbatim_spec_28_9                      64 B / 1 leaked      clean, 101 lines
//   per_iteration_local_static_callee_spec_28_14        7600 B / 300         clean
//   instance_method_callee_spec_28_14                   6400 B / 100         clean
//   expression_bodied_callee_iife_spec_27_5             6400 B / 100         clean
//   callee_lambda_captures_another_local_spec_27_5      6400 B / 100         clean
//   block_bodied_unrelated_lambda_spec_27_5             6400 B / 100         clean
//   method_own_type_param_named_local_spec_28_14        6400 B / 100         clean
//   method_own_type_param_lambda_literal_spec_28_14     6400 B / 100         clean
//   object_formal_type_test_spec_8_1                    6400 B / 100         clean
// Output is identical on both arms for every row.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// The program's stdout when it compiled, ran to exit code 0 and no sanitizer
// report fired (AddressSanitizer and LeakSanitizer both on); a marker
// otherwise.
std::string runCheckedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// `runBody` is the body of a static `Run(f: (int, int) -> int, i: int): int`;
// `Main` declares a fresh closure local per iteration, passes it to `Run`
// and prints the sum.
std::string loopProgram(const std::string& runBody) {
    return R"BZL(
module M;
public class C
  public static method Run(f: (int, int) -> int, i: int): int
)BZL" + runBody + R"BZL(
  end Run;
  public static method Main(): int
    var total := 0;
    for var i in 0..<100 do
      var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
      total := total + C.Run(g, i);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
)BZL";
}

}  // namespace

// #8482's repro, as filed: the callee's own closure local is conditionally
// reassigned and never touches `f`. Run(g, i) = 1 + i for even i, 2 + i for
// odd i.
BEZEL_TEST(closure_arg_callee_lambda_release_8482,
           issue_repro_verbatim_spec_28_9) {
    std::string expected;
    for (int i = 0; i < 100; ++i)
        expected += std::to_string((i % 2 == 0 ? 1 : 2) + i) + "\n";
    expected += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Run(f: (int, int) -> int, i: int): int
    var cmp: (int, int) -> int := lambda(x: int, y: int) => x * y;
    if i mod 2 = 0 then
      cmp := lambda(x: int, y: int) => y - x;
    end if;
    return cmp(1, 2) + i;
  end Run;
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<100 do
      Console.WriteLine(C.Run(g, i));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"),
                    expected);
}

// The same callee with a capturing closure local declared per iteration in
// the caller (cell + env + captured cell per iteration). Sum over 0..99 of
// (1 + i | 2 + i) = 4950 + 150.
BEZEL_TEST(closure_arg_callee_lambda_release_8482,
           per_iteration_local_static_callee_spec_28_14) {
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Run(f: (int, int) -> int, i: int): int
    var cmp: (int, int) -> int := lambda(x: int, y: int) => x * y;
    if i mod 2 = 0 then
      cmp := lambda(x: int, y: int) => y - x;
    end if;
    return cmp(1, 2) + i;
  end Run;
  public static method Main(): int
    var total := 0;
    for var i in 0..<100 do
      var g: (int, int) -> int := lambda(x: int, y: int) => x - y + i;
      total := total + C.Run(g, i);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
)BZL"),
                    std::string("5100\n"));
}

// An instance-method callee that also invokes `f`. f(2 + i, i) = 2.
BEZEL_TEST(closure_arg_callee_lambda_release_8482,
           instance_method_callee_spec_28_14) {
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class R
  public method Run(f: (int, int) -> int, i: int): int
    var cmp: (int, int) -> int := lambda(x: int, y: int) => x * y + i;
    return f(cmp(1, 2), i);
  end Run;
end R;
public class C
  public static method Main(): int
    var r := new R();
    var total := 0;
    for var i in 0..<100 do
      var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
      total := total + r.Run(g, i);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
)BZL"),
                    std::string("200\n"));
}

// An expression-bodied callee whose lambda is invoked in place.
// f(1, 2i) = 1 - 2i; summed: 100 - 9900.
BEZEL_TEST(closure_arg_callee_lambda_release_8482,
           expression_bodied_callee_iife_spec_27_5) {
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Run(f: (int, int) -> int, i: int): int =>
    f(1, (lambda(x: int) => x * 2)(i));
  public static method Main(): int
    var total := 0;
    for var i in 0..<100 do
      var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
      total := total + C.Run(g, i);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
)BZL"),
                    std::string("-9800\n"));
}

// The callee's lambda captures another local, not `f`. f(2i + 1, i) = i + 1.
BEZEL_TEST(closure_arg_callee_lambda_release_8482,
           callee_lambda_captures_another_local_spec_27_5) {
    BEZEL_EXPECT_EQ(runCheckedOut(loopProgram(
                        "    var k := i * 2;\n"
                        "    var h: () -> int := lambda() => k + 1;\n"
                        "    return f(h(), i);\n")),
                    std::string("5050\n"));
}

// A block-bodied lambda the walk must descend through statement by
// statement. f((1 + i) * 2, i) = 2 + i.
BEZEL_TEST(closure_arg_callee_lambda_release_8482,
           block_bodied_unrelated_lambda_spec_27_5) {
    BEZEL_EXPECT_EQ(runCheckedOut(loopProgram(
                        "    var k: (int) -> int := lambda(x: int) => do\n"
                        "      var t := x + i;\n"
                        "      return t * 2;\n"
                        "    end;\n"
                        "    return f(k(1), i);\n")),
                    std::string("5150\n"));
}

// A formal at a bare method-own type parameter instantiated to a function
// type (the #5129 arm of the same relief), with a named closure local.
// cmp(i) = 3i.
BEZEL_TEST(closure_arg_callee_lambda_release_8482,
           method_own_type_param_named_local_spec_28_14) {
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Run<U>(f: U, i: int): int
    var cmp: (int) -> int := lambda(x: int) => x * 3;
    return cmp(i);
  end Run;
  public static method Main(): int
    var total := 0;
    for var i in 0..<100 do
      var g: () -> int := lambda() => 7;
      total := total + C.Run<() -> int>(g, i);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
)BZL"),
                    std::string("14850\n"));
}

// The same callee given a lambda LITERAL: the call-end release of the
// literal (lowerCallDispatch's #5116 arm) reads the same callee-body walk.
BEZEL_TEST(closure_arg_callee_lambda_release_8482,
           method_own_type_param_lambda_literal_spec_28_14) {
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Run<U>(f: U, i: int): int
    var cmp: (int) -> int := lambda(x: int) => x * 3;
    return cmp(i);
  end Run;
  public static method Main(): int
    var total := 0;
    for var i in 0..<100 do
      total := total + C.Run<() -> int>(lambda() => 7, i);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
)BZL"),
                    std::string("14850\n"));
}

// An `Object` formal the callee only type-tests (the #8743 arm), beside an
// unrelated lambda. §8.1: a function type reaches `Object` through its
// reference representation, so the callee receives the very cell.
// `o is string` is false, so T = k(1) = 1 + i.
BEZEL_TEST(closure_arg_callee_lambda_release_8482,
           object_formal_type_test_spec_8_1) {
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method T(o: Object, i: int): int
    var k: (int) -> int := lambda(x: int) => x + i;
    if o is string then return 0; end if;
    return k(1);
  end T;
  public static method Main(): int
    var total := 0;
    for var i in 0..<100 do
      var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
      total := total + C.T(g, i);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
)BZL"),
                    std::string("5050\n"));
}
