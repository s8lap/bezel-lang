// Regression test for issue #4000: a statement-level throw in one branch of
// a plain if/else statement owns its named local on the throw path, but the
// normal path still reaches method-scope auto-disposal. The throw path must
// clear the source slot so the exception machinery's sole ownership is not
// followed by a second scope-end free (§28.3, §31.1, v0.294).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::codegen::CodegenOptions;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

JitResult runAotAsan(const std::string& source) {
  CodegenOptions opts;
  opts.sanitizeAddress = true;
  return runAot(source, opts);
}

}  // namespace

// FAIL-WITHOUT: the throw branch runs, the catch disposes the exception, and
// the method-scope auto-dispose then reads the same freed slot under ASan.
// PASS-WITH: the throw path clears the source slot; the catch returns 42 and
// the normal-path auto-dispose is a null-guarded no-op.
BEZEL_TEST(issue_4000, throw_statement_branch_caught_no_double_free_28_3_31_1) {
  JitResult r = runAotAsan(
      "module M; "
      "class Res "
      "  public tag: int; "
      "  public constructor(t: int) this.tag := t; end constructor; "
      "  destructor end destructor; "
      "end Res; "
      "class Prog "
      "  public static method Main(): int "
      "    var x := new Res(1); "
      "    var exc := new InvalidOperationException(\"nope\"); "
      "    var flag := false; "
      "    var r := 0; "
      "    try "
      "      var v := 0; "
      "      if flag then v := x.tag; else throw exc; end; "
      "      r := v; "
      "    catch var e: InvalidOperationException => r := 42;, "
      "    end try; "
      "    x.Dispose(); "
      "    return r; "
      "  end Main; "
      "end Prog;");
  BEZEL_EXPECT_TRUE(r.ran);
  BEZEL_EXPECT_EQ(r.exitCode, 42);
}
