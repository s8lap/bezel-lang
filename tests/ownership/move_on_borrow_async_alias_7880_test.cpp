// Regression pin for #7880 (spec §28.3 / §28.8.1, v0.357): `move` is not
// admitted on a borrow. The issue's literal repro takes a borrow alias
// `var t := t0;` of an owned `using var t0` Task and then transfers it with
// `using var u := move t;` - two owners of one Task, a double dispose at run
// time. The rule (borrow-state-local `move` rejection, fixed by commit
// e5e417c40; the plain-parameter sibling 2fa49247e and its family pin
// move_on_borrow_test.cpp do not name this repro) rejects the `move t` at compile time.
//
// ORACLE: `runAot`; the row asserts the SPECIFIC diagnostic, not mere failure.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

BEZEL_TEST(move_on_borrow_async_alias_7880, move_of_borrow_alias_of_owned_task_rejected_28_3) {
    JitResult r = runAot(
        "module M;\n"
        "public class Program\n"
        "public static async method W(): Task Console.WriteLine(\"w\"); end W;\n"
        "public static async method Main(): Task\n"
        " using var t0 := Program.W(); var t := t0; using var u := move t; await u;\n"
        " Console.WriteLine(\"END\"); end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("`move` is not admitted on a borrow") != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
}
