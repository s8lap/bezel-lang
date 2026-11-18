// fix #8926 / §28.3 *Captured owned bindings* + §31.1 *The throw operand*
// (spec v0.359): `throw c` in a NESTED closure, where `c` is an `owned`
// PARAMETER of the ENCLOSING closure.
//
// §28.3: "Inside a closure, a captured owned binding — an owned local or
// `owned` parameter of the enclosing body — is uniformly in borrow state: ...
// every other owned-consuming use of it are compile errors". §31.1: "The
// operand is consumed as an owned value". The "enclosing body" of a nested
// closure is the enclosing closure's body, so the #7920 rule ("cannot throw
// captured owned binding `c` inside a closure") covers it. The METHOD-level
// twin is rejected; the enclosing-closure form compiled (rc=0) and ran.
//
// #8873 closed the OWNED LOCAL half of this shape; the `owned` PARAMETER
// half was left: `reportEnclosingLambdaOwnedThrow` reads the binding's
// history through `lambdaBodyLocalHoldsBorrow`, which finds a BINDING
// STATEMENT — a parameter has none — and a closure's `ParameterSymbol`
// carries no declaration location either (sema records only its name and
// modifier), so the owner could not be resolved by source range. The owner is
// now read from `ctx_.lambdaParams`, and the binding is OWNED iff the
// parameter's own modifier says so.
//
// Each row is red on the base, asserts the specific diagnostic AND its
// location (the operand of the `throw`), and is the issue's own repro shape
// (row 1 verbatim). Base-green controls (the throwing closure's OWN `owned`
// parameter, a non-`owned` parameter, the METHOD-level twin) are corpus
// candidates.
//
// per §28.3, §31.1, v0.359, #8926

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

const char* kR = R"(
module M;
public class R: Exception
  public constructor() end constructor;
end R;
)";

// ROW 1 — the issue's own repro: the `owned` parameter of the ENCLOSING
// closure is thrown by a nested closure (the operand `c` is at line 10,
// column 42).
BEZEL_TEST(nested_lambda_captured_owned_param_throw_8926,
           nested_closure_throw_enclosing_closures_owned_parameter_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(owned c: R): int => do
      var g := lambda(): int => do throw c; end;
      _ := g();
      return 0;
    end;
    try _ := f(new R()); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":10:42: cannot throw captured owned binding `c` inside a closure"));
}

// ROW 2 — the `!` spelling: §17.8 classifies `c!` by its position exactly as
// the bare read `c`, so `throw c!` is the same question (the operand `c` is
// at line 10, column 42).
BEZEL_TEST(nested_lambda_captured_owned_param_throw_8926,
           nested_closure_throw_bang_of_enclosing_closures_owned_parameter_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(owned c: R): int => do
      var g := lambda(): int => do throw c!; end;
      _ := g();
      return 0;
    end;
    try _ := f(new R()); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":10:42: cannot throw captured owned binding `c` inside a closure"));
}

// ROW 3 — one more level: the innermost closure throws the OUTERMOST
// closure's `owned` parameter (the operand `c` is at line 11, column 44).
BEZEL_TEST(nested_lambda_captured_owned_param_throw_8926,
           three_level_nested_closure_throw_outermost_closures_owned_parameter_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(owned c: R): int => do
      var g := lambda(): int => do
        var h := lambda(): int => do throw c; end;
        _ := h();
        return 0;
      end;
      _ := g();
      return 0;
    end;
    try _ := f(new R()); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":11:44: cannot throw captured owned binding `c` inside a closure"));
}

}  // namespace
