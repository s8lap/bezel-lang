// `using var x := a!` over an owned inline array `[2 owned]Res?` MOVES the
// array into `x`, exactly as the bare `using var x := a` does (issue #7860;
// spec §17.8 / §15.3 / §28.10, v0.357).
//
// §17.8: "Over a named owned binding, `x!` is therefore classified by its
// position exactly as the bare read `x` is". §15.3: "Assignment transfers the
// value from source to destination, leaving the source in moved-from state.
// Reading from a moved-from location is a compile error."
//
// STATUS. Issue #7860 was measured on a pre-d42cb5ed1 tree, where
// noteMoveOnlyArrayWholeMoveRead required a bare IdentifierExpr, so the later
// read `var n := a;` was accepted and the program freed twice. fix #8375/#8377
// (d42cb5ed1) peeled the `!` and fixed it as a side effect, but pinned only the
// call-argument and plain-local-decl positions; the `using var` initializer
// position was unpinned. This test-only commit pins it with the issue's own
// repro (matrix a03 / a01 of the R37 U5 bang-pin lane).
//
// per §17.8, §15.3, §28.10, v0.357

#include <string>

#include "support/aot_support.hpp"  // runAot / runAotChecked -- strict AOT
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The issue's program; `tail` is what follows `using var x := a!;` in T.
std::string prog(const std::string& tail) {
    return std::string(
               "module M;\n"
               "public class Res\n"
               "  public static Count: int := 0;\n"
               "  public constructor() end constructor;\n"
               "  destructor Res.Count := Res.Count + 1; end destructor;\n"
               "end Res;\n"
               "public class Program\n"
               "  public static method T()\n"
               "    var a: [2 owned]Res? := [new Res(), null];\n"
               "    using var x := a!;\n") +
           tail +
           "  end T;\n"
           "  public static method Main(): int\n"
           "    for var i in 0..<100 do\n"
           "      T();\n"
           "    end;\n"
           "    Console.WriteLine($\"{Res.Count}\");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

bool hasMovedOutErrorOn(const JitResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("moved out") != std::string::npos &&
            d.message.find("`" + name + "`") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// The issue's repro: the later read of `a` after `using var x := a!;` is a
// moved-from read (the bare spelling is rejected identically).
BEZEL_TEST(using_var_bang_move_only_array_7860,
           spec_17_8_read_after_using_var_bang_array_is_rejected) {
    auto r = runAot(prog("    var n := a;\n"));
    BEZEL_EXPECT_TRUE(hasMovedOutErrorOn(r, "a"));
}

// Same rejection through an element read, the borrow-shaped later use.
BEZEL_TEST(using_var_bang_move_only_array_7860,
           spec_17_8_element_read_after_using_var_bang_array_is_rejected) {
    auto r = runAot(prog("    var n := a[0];\n"));
    BEZEL_EXPECT_TRUE(hasMovedOutErrorOn(r, "a"));
}

// ADMITTING TWIN -- with no later read the move is legal and the element is
// disposed exactly once per iteration (100 x one non-null element), with no
// double free under ASan/LSan.
BEZEL_TEST(using_var_bang_move_only_array_7860,
           spec_17_8_using_var_bang_array_without_later_read_disposes_once) {
    auto r = runAotChecked(prog(""));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("100\n"));
}
