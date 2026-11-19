// Ownership — inside a LAMBDA body, a `catch` arm that reads a local moved
// in the `try` was admitted; the byte-identical METHOD body is rejected.
// Issue #8940.
//
// SPEC: §28.3 "Local variables, ownership, and borrows" (v0.359), K1 item 3:
//   "**moved-from** — the obligation has transferred out; the binding or slot
//    is unreadable until reassigned, and discharge machinery skips it"
// A `catch` arm is reached by the exception edge out of the `try` body, and
// §28.8.1 / §22.9 fix the state on that edge: "A call that raises does not
// give the argument back: the transfer completes at call entry, the binding
// is still moved-from in the `catch` arm" (docs/Bezel_For_Code_Generation.md
// §`move` cites §28.8.1, §22.9 for exactly that sentence). The method body's
// own dataflow therefore sees the binding as moved-from in the arm.
//
// ROOT CAUSE: `MethodOwnership::applyUsingVarBorrowPinInLambdaStmt`'s
// `ast::TryStmt` arm restored the PRE-try state snapshot before each catch
// arm (#3951's conservative stance), so the arm's read of a local the `try`
// body moved saw the pre-try (Owned) state and was admitted.
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

bool hasMovedOut(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
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
    "  public static method Take(owned r: Res): int return r.tag; end Take; "
    "end Sink; ";

// The `try` body moves `x`; the `catch` arm reads it.
std::string catchReadProgram() {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int "
           "    var f := lambda(): int => do "
           "      var x := new Res(1); var k := 0; "
           "      try k := Sink.Take(move x); "
           "      catch e: Exception => k := x.tag; end; "
           "      return k; "
           "    end; "
           "    return f(); "
           "  end Main; "
           "end C;";
}

}  // namespace

BEZEL_TEST(lambda_body_catch_read_after_move_8940,
           catch_arm_read_of_moved_local_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(catchReadProgram());
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOut(r));
}

BEZEL_TEST(lambda_body_catch_read_after_move_8940,
           aot_catch_arm_read_of_moved_local_is_a_compile_error_28_3) {
    JitResult jr = runAot(catchReadProgram());
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
}
