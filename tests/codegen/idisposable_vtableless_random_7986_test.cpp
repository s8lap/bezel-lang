// Regression pin for #7986 (spec §8.7, §5.1, v0.357): a vtable-less stdlib
// class (`Random`) passed to an `owned d: IDisposable` slot and disposed
// there. Before the fix the slot's `d.Dispose()` dispatched through a vtable
// the cell does not have and SEGV'd. Fixed by f39912506 (narrow #8000: seven
// vtable-less builtin classes, Random among them, are tagged with a word-0
// vtable so the slot dispatches real Dispose).
//
// ORACLE: `runAot` (AOT, native exec); asserts stdout and exit code.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

BEZEL_TEST(idisposable_vtableless_random_7986, random_disposed_through_owned_idisposable_slot) {
    JitResult r = runAot(
        "module M;\n"
        "public class G\n"
        "  public static method Take(owned d: IDisposable)\n"
        "    d.Dispose();\n"
        "  end Take;\n"
        "end G;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    G.Take(new Random());\n"
        "    Console.WriteLine(\"OK\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("OK\n"));
}
