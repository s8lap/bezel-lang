// Ownership — inside a LAMBDA body, the lambda's OWN `owned` parameter moved
// in the `try` and read in the `catch` arm was admitted (and crashed at run
// time); the byte-identical METHOD body is rejected. Issue #8992 (the
// parameter sibling of #8940's local-variable fix).
//
// SPEC: §28.3 "Local variables, ownership, and borrows" (v0.359), K1 item 3:
//   "**moved-from** — the obligation has transferred out; the binding or slot
//    is unreadable until reassigned, and discharge machinery skips it"
// and §28.8 / §28.8.1: an `owned` parameter is an obligation the callee holds
// from entry, so `move p` transfers it exactly as `move` of an owned local
// does; §22.9 fixes the exception-edge state: "A call that raises does not
// give the argument back: the transfer completes at call entry, the binding
// is still moved-from in the `catch` arm".
//
// ROOT CAUSE: the lambda-body pre-pass
// (`MethodOwnership::applyUsingVarBorrowPinInLambdaBody`) seeded only the
// CAPTURED locals into its state map, never the lambda's own `owned`
// parameters, so `move p` recorded nothing and the arm's read of `p` saw an
// untracked name.
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

// The METHOD twin's exact diagnostic.
const char* kMovedOut =
    "owned value in `p` was already moved out at this point (§28.3)";

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

// The lambda's own `owned` parameter is moved in the `try` and read in the
// `catch` arm.
std::string ownedParamProgram() {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int "
           "    var f := lambda(owned p: Res): int => do "
           "      var k := 0; "
           "      try k := Sink.TakeBoom(move p); "
           "      catch e: Exception => k := p.tag; end; "
           "      return k; "
           "    end; "
           "    return f(new Res(1)); "
           "  end Main; "
           "end C;";
}

}  // namespace

BEZEL_TEST(lambda_body_catch_owned_param_read_after_move_8992,
           catch_arm_read_of_moved_owned_param_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(ownedParamProgram());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOut(r.diagnostics));
}

BEZEL_TEST(lambda_body_catch_owned_param_read_after_move_8992,
           aot_catch_arm_read_of_moved_owned_param_is_a_compile_error_28_3) {
    JitResult jr = runAot(ownedParamProgram());
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(hasMovedOut(jr.diagnostics));
}
