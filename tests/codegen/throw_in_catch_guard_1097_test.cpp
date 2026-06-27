// Regression tests for #1097 (§31.5 / §31.2.1): a `throw` expression used as a
// catch-arm `when` guard crashed codegen — the throw's allocated object pointer
// was emitted as the i1 branch condition (`br ptr poison` → IR verification
// failure). A `throw` is a bottom-typed expression usable in any expression
// position (§31.5), and a guard that throws supersedes the in-flight exception
// (§31.2.1). The fix detects the guard diverged (threw) and skips the branch —
// the throw propagates via its own unwind branch. Found by adversarial
// discovery sweep #7.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
const char* E2 =
    "module M; class E1 end E1; class E2 end E2; ";
}  // namespace

// #1097: a `throw` in a `when` guard compiles (was an IR-verification crash).
// Here the throwing-guard arm is never triggered (a different exception is
// thrown and caught by a later arm), proving the throw-in-guard lowers cleanly.
BEZEL_TEST(throw_in_catch_guard_1097, throw_guard_arm_not_triggered) {
    BEZEL_EXPECT_EQ(run(
        std::string(E2) +
        "class C public static method Main(): int "
        "  try "
        "    throw new E2(); "
        "  catch E1 when throw new E1() => return 1; "    // guard never runs
        "  catch E2 => return 7; "                        // E2 caught here
        "  end; "
        "  return 0; "
        "end Main; end C;"), 7);
}

// §31.2.1: when the guard DOES throw, the new exception supersedes the in-flight
// one and propagates — here an outer try catches it (program runs to
// completion, proving the supersede semantics, not just no-crash).
BEZEL_TEST(throw_in_catch_guard_1097, guard_throw_supersedes_outer_catches) {
    BEZEL_EXPECT_EQ(run(
        std::string(E2) +
        "class C public static method Main(): int "
        "  try "
        "    try "
        "      throw new E1(); "
        "    catch E1 when throw new E2() => return 1; "  // guard throws E2
        "    end; "
        "  catch E2 => return 9; "                        // outer catches E2
        "  end; "
        "  return 0; "
        "end Main; end C;"), 9);
}

// A NORMAL (non-throwing) guard that is true selects its arm.
BEZEL_TEST(throw_in_catch_guard_1097, normal_guard_true) {
    BEZEL_EXPECT_EQ(run(
        std::string(E2) +
        "class C public static method Main(): int "
        "  var n := 5; "
        "  try throw new E1(); "
        "  catch E1 when n > 3 => return 1; "
        "  catch E1 => return 2; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// A normal guard that is false falls through to the next arm.
BEZEL_TEST(throw_in_catch_guard_1097, normal_guard_false_falls_through) {
    BEZEL_EXPECT_EQ(run(
        std::string(E2) +
        "class C public static method Main(): int "
        "  var n := 1; "
        "  try throw new E1(); "
        "  catch E1 when n > 3 => return 1; "
        "  catch E1 => return 2; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 2);
}
