// fix #8958 / §28.3 *Captured owned bindings* + §31.1 *The throw operand*
// (spec v0.359): a CAPTURED owned binding thrown through a CONDITIONAL operand
// inside a closure.
//
// §28.3: "Inside a closure, a captured owned binding — an owned local or
// `owned` parameter of the enclosing body — is uniformly in borrow state: ...
// every other owned-consuming use of it are compile errors". §31.1: "The
// operand is consumed as an owned value".
//
// THE DEFECT. The #7920 rule (`reportCapturedThrowOperand`,
// src/ownership/Ownership_Checks.cpp) rejects `throw c;` of a CAPTURED OWNED
// binding — "cannot throw captured owned binding `c` inside a closure" — but
// it classified the operand only when it was a BARE IDENTIFIER (it peels a
// `!`, but not a composite expression). A captured owned binding that reaches
// the `throw` through a COMPOSITE operand was therefore admitted, and the
// program died at run time with a double dispose.
//
// MEASURED on r30-c1 @ 3c85905f0, the issue's literal repro: `bezelc` exits
// 0, the program prints "caught" and then dies with a heap-use-after-free
// (ASan: "ERROR: AddressSanitizer: heap-use-after-free"); without the
// sanitizer it dies with SIGSEGV (rc=139). The runtime disposed the thrown
// instance after the catch arm and the enclosing scope disposed it again.
//
// THE FIX. The composite operand's arms are classified by the SAME predicates
// the bare form uses (`isCapturedOwnedBinding` — an enclosing BODY binding
// still in the Owned state — and `reportEnclosingLambdaOwnedThrow` — an owned
// local / `owned` parameter of an ENCLOSING closure), and the diagnostic is
// the direct form's own (`reportCapturedOwnedConsume`, verb "throw"), so the
// two spellings draw byte-identical text.
//
// Each row is red on the base (the base COMPILES these programs: the shape
// was admitted) and asserts the specific diagnostic AND its location (the
// offending binding's own position, the direct form's position). The
// admitting twins — the lambda's OWN owned local through the same conditional,
// a conditional over two FRESH constructions, the closure's OWN `owned`
// parameter, the METHOD-body twin, and the `??` spelling — are corpus
// candidates, not rows: they are green on both arms.
//
// per §28.3, §31.1, v0.359, #8958

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

// ROW 1 — the issue's own repro: a METHOD-level owned local captured by a
// closure and thrown through a conditional (the offending binding `c` is at
// line 11, column 28).
BEZEL_TEST(captured_owned_conditional_throw_8958,
           block_closure_throw_conditional_of_captured_owned_local_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var c := new R(); var n := 1;
    var f := lambda(): int => do
      throw (if n > 0 then c else c);
    end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":11:28: cannot throw captured owned binding `c` inside a closure"));
}

// ROW 2 — the `owned` PARAMETER of the enclosing body: §28.3 names it beside
// the owned local ("an owned local or `owned` parameter of the enclosing
// body"), so the conditional spelling of it is the same shape (the offending
// binding `c` is at line 11, column 28).
BEZEL_TEST(captured_owned_conditional_throw_8958,
           block_closure_throw_conditional_of_captured_owned_parameter_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Thrower(owned c: R): int
    var n := 1;
    var f := lambda(): int => do
      throw (if n > 0 then c else c);
    end;
    try f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Thrower;
  public static method Main(): int
    _ := Thrower(new R());
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":11:28: cannot throw captured owned binding `c` inside a closure"));
}

// ROW 3 — the ENCLOSING CLOSURE's owned local, thrown by a NESTED closure
// through a conditional (fix #8873's captured-local shape in its composite
// spelling; the offending binding `c` is at line 13, column 30).
BEZEL_TEST(captured_owned_conditional_throw_8958,
           nested_closure_throw_conditional_of_enclosing_closures_owned_local_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    var n := 1;
    var f := lambda(): int => do
      var c := new R();
      var g := lambda(): int => do
        throw (if n > 0 then c else c);
      end;
      _ := g();
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
        r, ":13:30: cannot throw captured owned binding `c` inside a closure"));
}

}  // namespace
