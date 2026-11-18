// fix #7920 / §28.3 *Captured owned bindings* + §31.1 *The throw operand*
// (spec v0.359): `throw c` over a captured owned binding `c` inside a closure.
//
// §31.1: "The operand is consumed as an owned value under the ownership
// rules", and `throw x` "is a source-visible kill". §28.3: inside a closure a
// captured owned binding is in borrow state and "every other owned-consuming
// use of it [is a] compile error". MEASURED on b279e8daf: all three spellings
// compiled (rc=0): the closure threw the enclosing scope's owned value, which
// the runtime then disposed after the catch arm while the enclosing scope
// still held the disposal obligation.
//
// Each row is red on the base. The admitting twins (a closure's own local, a
// fresh `throw new`, a method-level throw, a closure's own `owned` parameter)
// are corpus candidates.
//
// per §28.3, §31.1, v0.359, #7920

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

constexpr const char* kCapturedThrow = "cannot throw captured owned binding";

const char* kR = R"(
module M;
public class R: Exception
  public constructor() end constructor;
end R;
)";

// ROW 1 — a block-bodied closure, `throw c;` as a statement, over a captured
// plain `var`.
BEZEL_TEST(captured_owned_throw_in_closure_7920,
           block_closure_throw_statement_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    try
      var c := new R();
      var f := lambda(): int => do throw c; end;
      f();
    catch var e: R => Console.Write("");
    end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kCapturedThrow));
}

// ROW 2 — an expression-bodied closure, `=> throw c`, over a captured
// `using var`.
BEZEL_TEST(captured_owned_throw_in_closure_7920,
           expression_closure_throw_rejected_spec_28_3_v0_359) {
    std::string src = std::string(kR) + R"(
class Program
  public static method Main(): int
    try
      using var c := new R();
      var f := lambda(): int => throw c;
      f();
    catch var e: R => Console.Write("");
    end;
    return 0;
  end Main;
end Program;
)";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kCapturedThrow));
}

}  // namespace
