// Ownership — inside a LAMBDA body, a `catch` arm's `when` GUARD that reads a
// local moved in the `try` was admitted (and crashed at run time); the
// byte-identical METHOD body is rejected. Issue #8991 (the guard sibling of
// #8940's arm-body fix).
//
// SPEC: §28.3 "Local variables, ownership, and borrows" (v0.359), K1 item 3:
//   "**moved-from** — the obligation has transferred out; the binding or slot
//    is unreadable until reassigned, and discharge machinery skips it"
// The `when` guard runs on the same exception edge as the arm body (§31.2.1:
// the guard is an ordinary expression evaluated on entry to the arm), and
// §28.8.1 / §22.9 fix the state on that edge: "A call that raises does not
// give the argument back: the transfer completes at call entry, the binding
// is still moved-from in the `catch` arm". So a guard that reads the local is
// a read of a moved-from binding.
//
// ROOT CAUSE: `MethodOwnership::applyUsingVarBorrowPinInLambdaStmt`'s
// `ast::TryStmt` arm only COLLECTED nested lambdas out of `arm.guard`; the
// guard expression itself was never run through the read/discharge check the
// method body's CFG applies to `block.catchGuard`.
//
// MEASURED PRE-FIX: the program below compiled rc=0 with no diagnostic.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// The METHOD twin's exact diagnostic — the lambda form must draw the same
// text, so this needle is the whole claim, not a prefix of it.
const char* kMovedOut =
    "owned value in `x` was already moved out at this point (§28.3)";

template <class Diags>
bool hasMovedOut(const Diags& ds) {
    for (const auto& d : ds.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(kMovedOut) != std::string::npos)
            return true;
    return false;
}

const char* kPrelude =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Sink "
    "  public static method TakeBoom(owned r: Res): int "
    "    do throw new InvalidOperationException(\"boom\"); end; "
    "  end TakeBoom; "
    "end Sink; ";

// The `try` body moves `x`; the catch arm's `when` guard reads it.
std::string guardReadProgram() {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int "
           "    var f := lambda(): int => do "
           "      var x := new Res(1); var k := 0; "
           "      try k := Sink.TakeBoom(move x); "
           "      catch e: Exception when x.tag > 0 => k := 1; end; "
           "      return k; "
           "    end; "
           "    return f(); "
           "  end Main; "
           "end C;";
}

}  // namespace

BEZEL_TEST(lambda_body_catch_guard_read_after_move_8991,
           guard_read_of_moved_local_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(guardReadProgram());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOut(r.diagnostics));
}

BEZEL_TEST(lambda_body_catch_guard_read_after_move_8991,
           aot_guard_read_of_moved_local_is_a_compile_error_28_3) {
    JitResult jr = runAot(guardReadProgram());
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(hasMovedOut(jr.diagnostics));
}
