// Ownership — inside a LAMBDA body, a `return` whose operand reads a moved-from
// owned local was admitted; the byte-identical METHOD body is rejected.
// Issue #8902.
//
// SPEC: §28.3 "Local variables, ownership, and borrows", K1 item 3 (v0.359):
//   "**moved-from** — the obligation has transferred out; the binding or slot
//    is unreadable until reassigned, and discharge machinery skips it"
// A `return` operand is a read position like any other.
//
// ROOT CAUSE: `MethodOwnership::applyUsingVarBorrowPinInLambdaStmt`'s
// ReturnStmt arm only collected nested lambdas; unlike its AssignmentStmt arm
// it never called `checkRead` / `recordMovesIn` on the operand.
//
// MEASURED PRE-FIX: each program below compiled rc=0 with no diagnostic.

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

bool hasMovedOut(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("owned value in `x` was already moved out at "
                           "this point (§28.3)") != std::string::npos)
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

std::string wrap(const std::string& lambdaBody) {
    return std::string(kPrelude) +
           "class C "
           "  public static method Main(): int "
           "    var f := lambda(): int => do " +
           lambdaBody +
           "    end; "
           "    return f(); "
           "  end Main; "
           "end C;";
}

const char* kConsumed = "var x := new Res(1); var k := Sink.Take(move x); ";

}  // namespace

BEZEL_TEST(lambda_body_return_read_after_move_8902,
           return_member_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        wrap(std::string(kConsumed) + "return x.tag + k; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOut(r));
}

BEZEL_TEST(lambda_body_return_read_after_move_8902,
           return_call_argument_read_after_move_rejected_28_3) {
    OwnershipResult r = parseAndCheckOwnership(wrap(
        std::string(kConsumed) + "return Sink.Take(move x) + k; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMovedOut(r));
}

BEZEL_TEST(lambda_body_return_read_after_move_8902,
           aot_return_read_after_move_is_a_compile_error_28_3) {
    JitResult jr =
        runAot(wrap(std::string(kConsumed) + "return x.tag + k; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
}

// CONTROL: a lambda returning a local that was NOT moved stays admitted.
BEZEL_TEST(lambda_body_return_read_after_move_8902,
           control_return_unmoved_local_admitted_28_3) {
    JitResult jr = runAot(wrap(
        "var x := new Res(4); var k := Sink.Take(move x); "
        "var z := new Res(2); return z.tag + k; "));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 6);
}
