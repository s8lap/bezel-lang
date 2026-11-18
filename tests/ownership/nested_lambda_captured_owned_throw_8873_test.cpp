// fix #8873 / §28.3 *Captured owned bindings* + §31.1 *The throw operand*
// (spec v0.359): `throw c` in a NESTED lambda, where `c` is an OWNED local
// declared in the ENCLOSING lambda's body.
//
// §28.3: "Inside a closure, a captured owned binding — an owned local or
// `owned` parameter of the enclosing body — is uniformly in borrow state: ...
// every other owned-consuming use of it are compile errors". §31.1: "The
// operand is consumed as an owned value". The "enclosing body" of a nested
// closure is the enclosing lambda's body; the METHOD-level twin is rejected by
// #7920 ("cannot throw captured owned binding `c` inside a closure").
// Base-green controls (the inner lambda's own owned local, `throw new R()`)
// are corpus candidates.
//
// per §28.3, §31.1, v0.359, #8873

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

// ROW 1 — the issue's own repro (operand `c` at line 9, column 42).
BEZEL_TEST(nested_lambda_captured_owned_throw_8873,
           nested_closure_throw_enclosing_closures_owned_local_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(): int => do
      var c := new R();
      var g := lambda(): int => do throw c; end;
      g();
      return 0;
    end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot throw captured owned binding `c` inside a closure"));
}

// ROW 2 — one more level: the innermost closure throws the OUTERMOST
// closure's owned local.
BEZEL_TEST(nested_lambda_captured_owned_throw_8873,
           three_level_nested_closure_throw_outermost_closures_owned_local_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var f := lambda(): int => do
      var c := new R();
      var g := lambda(): int => do
        var h := lambda(): int => do throw c; end;
        h();
        return 0;
      end;
      g();
      return 0;
    end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "cannot throw captured owned binding `c` inside a closure"));
}

}  // namespace
