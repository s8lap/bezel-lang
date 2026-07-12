// Codegen slice CG11 — `break` and `continue` in `while` loops (§A.6).
// `break` branches to the innermost loop's exit block; `continue` to its
// condition block. Both terminate the current basic block, so statements
// after them in the same block are dead. Observable via the int exit code.
//
// (`for ... in` iteration is deferred — it needs the stdlib iterable
// protocol; this slice covers the `while`-loop transfer statements only.)

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg11, break_exits_loop_early) {
    // Accumulate 1..5, then break when i exceeds 5.
    BEZEL_EXPECT_EQ(run(
        "var i := 0; var s := 0; "
        "while i < 100 do "
        "  i := i + 1; "
        "  if i > 5 then break; end; "
        "  s := s + i; "
        "end; "
        "return s;"), 15);
}

BEZEL_TEST(codegen_cg11, continue_skips_iteration) {
    // Sum the even values in 1..10 by continuing past the odd ones.
    BEZEL_EXPECT_EQ(run(
        "var i := 0; var s := 0; "
        "while i < 10 do "
        "  i := i + 1; "
        "  if i mod 2 <> 0 then continue; end; "
        "  s := s + i; "
        "end; "
        "return s;"), 30);
}

BEZEL_TEST(codegen_cg11, break_terminates_infinite_loop) {
    // `while true` is exited only by the break.
    BEZEL_EXPECT_EQ(run(
        "var i := 0; "
        "while true do "
        "  i := i + 1; "
        "  if i = 7 then break; end; "
        "end; "
        "return i;"), 7);
}

BEZEL_TEST(codegen_cg11, break_affects_innermost_loop_only) {
    // The inner loop breaks at j = 2 (after one increment of count) each
    // of the three outer iterations.
    BEZEL_EXPECT_EQ(run(
        "var i := 0; var count := 0; "
        "while i < 3 do "
        "  var j := 0; "
        "  while j < 10 do "
        "    j := j + 1; "
        "    if j = 2 then break; end; "
        "    count := count + 1; "
        "  end; "
        "  i := i + 1; "
        "end; "
        "return count;"), 3);
}

BEZEL_TEST(codegen_cg11, continue_in_inner_loop_only) {
    // Inner loop sums j in 1..4 skipping j = 2 (→ 1+3+4 = 8); outer runs
    // twice, so 16. The `continue` must rejoin the inner condition, not
    // the outer one.
    BEZEL_EXPECT_EQ(run(
        "var i := 0; var s := 0; "
        "while i < 2 do "
        "  var j := 0; "
        "  while j < 4 do "
        "    j := j + 1; "
        "    if j = 2 then continue; end; "
        "    s := s + j; "
        "  end; "
        "  i := i + 1; "
        "end; "
        "return s;"), 16);
}
