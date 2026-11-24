// Ownership — a move-only local declared in a LAMBDA body and consumed by a
// NESTED lambda is rejected, exactly like the method-body twin (issue #8987).
//
// SPEC: §28.3 "Local variables, ownership, and borrows", *Captured owned
// bindings* (v0.359):
//
//   "Inside a closure, a captured owned binding — an owned local or `owned`
//    parameter of the enclosing body — is uniformly in borrow state: reads
//    borrow (the producers rows above), and assignment to it, a marked
//    transfer from it, an explicit `Dispose()` or discard of it, and every
//    other owned-consuming use of it are compile errors — a closure cannot
//    consume, reassign, or dispose a captured owned binding."
//
// The "enclosing body" of a NESTED closure is the ENCLOSING LAMBDA's body, so
// a move-only local declared there and consumed by the nested closure is the
// same rule the METHOD spelling draws.
//
// §28.9 (v0.359): "Capturing a *move-only* local (an owning inline array,
// §15.3) follows the same rule: the closure captures a borrow whose source is
// the local's enclosing scope, and may not outlive it ... a move-only capture
// cannot be a copy (copying is forbidden) and cannot be a move (the local is
// still in scope under its name ...)". §15.3 makes the PLAIN binding of a
// move-only value a MOVE ("copies are forbidden, so the plain binding can only
// move"), so `var x := b;` inside the nested closure is an owned-consuming
// use of the enclosing lambda's `b`.
//
// ROOT CAUSE: src/ownership/Ownership_Checks.cpp,
// MethodOwnership::reportMoveOnlyChoiceMoveInClosure — the check every
// owned-consuming position inside a closure body calls — resolved the binding
// against the METHOD frame's `states_` (BlockState map). A local declared in
// an enclosing LAMBDA's body has no entry in any method-frame state map, so
// the predicate was false and the nested closure's consume of it was
// admitted: the closure's taker and the enclosing lambda's scope-end pass
// both disposed the one payload.
//
// MEASURED PRE-FIX: the program below compiled rc=0 with no diagnostic, and
// the run under `--sanitize=address` was a heap-use-after-free (a
// twice-disposed slot).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The METHOD twin's exact diagnostic — the lambda spelling must draw the
// same text, so this needle is the whole claim, not a prefix of it.
const char* kCapturedTransfer =
    "cannot transfer captured owned binding `b` inside a closure — a captured "
    "owned binding is borrow-state throughout the closure (§28.3, v0.184)";

const char* kPrelude = R"BZL(module M;
public class Res
  public static Count: int := 0;
  public N: int := 7;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;
)BZL";

// The ISSUE's literal repro: `b` is declared in the OUTER LAMBDA's body and
// the NESTED lambda `g` moves it.
std::string nestedMoveProgram() {
    return std::string(kPrelude) + R"BZL(public class Program
  public static method Main(): int
    var outer := lambda(): int => do
      var b: [1 owned]Res? := [new Res()];
      var g := lambda(): int => do var x := b; return 1; end;
      return g();
    end;
    Console.WriteLine($"{outer()}");
    return 0;
  end Main;
end Program;
)BZL";
}

// The ADMITTING twin: the nested lambda only READS the enclosing lambda's
// move-only local (`b[0]!.N` borrows the element), so nothing is consumed
// and the program must still compile, run and dispose the payload exactly
// once per iteration.
std::string nestedReadProgram() {
    return std::string(kPrelude) + R"BZL(public class Program
  public static method Step(): int
    var outer := lambda(): int => do
      var b: [1 owned]Res? := [new Res()];
      var g := lambda(): int => do return b[0]!.N; end;
      return g();
    end;
    return outer();
  end Step;
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do
      s := s + Step();
    end;
    Console.WriteLine($"{Res.Count} {s}");
    return 0;
  end Main;
end Program;
)BZL";
}

}  // namespace

BEZEL_TEST(nested_lambda_move_enclosing_lambda_move_only_local_8987,
           aot_nested_lambda_move_of_enclosing_lambda_local_is_a_compile_error_28_3) {
    // The issue's repro: the nested closure's `var x := b;` moves what the
    // ENCLOSING LAMBDA's scope still disposes. The method-body twin draws the
    // same diagnostic, byte for byte.
    const JitResult r = runAot(nestedMoveProgram());
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kCapturedTransfer));
}

BEZEL_TEST(nested_lambda_move_enclosing_lambda_move_only_local_8987,
           aot_nested_lambda_read_of_enclosing_lambda_local_still_runs_28_3) {
    // The ADMITTING twin, as the ASan/LSan-leg boundary control: a nested
    // closure that only READS the enclosing lambda's move-only local consumes
    // nothing. 200 iterations under the checked AOT oracle: one disposal per
    // iteration, no leak, no use-after-free. A fix that rejected the READ
    // would turn this row red.
    const JitResult r = runAotChecked(nestedReadProgram());
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "200 1400\n");
}
