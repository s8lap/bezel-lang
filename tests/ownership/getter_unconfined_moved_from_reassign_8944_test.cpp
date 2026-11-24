// fix #8944 (§28.3.3 *State match on reassignment* item 2, §28.7, v0.359): a
// property getter's block body that reassigns a MOVED-FROM owned local frees
// the value that was moved out of it.
//
// Spec (v0.359):
//  - §28.3.3 item 2: "A **moved-from** LHS is an owned-state LHS holding no
//    live value: it requires an owned RHS and displaces nothing". The store
//    `t := new Cell(2);` after `var u := move t;` therefore disposes nothing.
//  - §28.7: drop-on-overwrite runs only if the previous value is live
//    ("non-null and not moved-from"); "a moved-from or null location therefore
//    stores without disposal".
//  - §28.3.10: every owned value is disposed exactly once on every path.
//  - §28.9: the two accessor body spellings are indistinguishable at a read
//    site, so the getter must behave like its method twin.
//
// THE DEFECT. A getter body that the #8767 confinement proof does not accept
// (here a `using var` local, which the proof denies) never runs the method
// ownership dataflow, so `AssignmentStmt::lhsMovedFromAtStore` is never set and
// codegen's drop-on-overwrite pass frees the pointer the plain `var` slot still
// holds: the value `u` now owns. MEASURED on r30-c1 (staging base of lane
// r43-c10-8944), the issue's repro, 200 reads: AddressSanitizer
// heap-use-after-free in `bz$prop$V$get`; the method twin prints `600 / 0`.
//
// ORACLE. `runAotCheckedLenient`: generated-code AddressSanitizer with
// LeakSanitizer OFF. A body the #8767 proof does not accept arms no scope-end
// free (the documented, safe-direction bounded leak of `t` and `u`), so these
// rows pin the ABSENCE OF THE USE-AFTER-FREE, not a leak count, and the
// program does not print `Cell.n`. Each row asserts the exact stdout (the sum
// over 200 reads, which a read of freed memory corrupts or traps) and exit
// code 0.
//
// per §28.3.3, §28.7, §28.3.10, §28.9 (v0.359)

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotCheckedLenient;

namespace {

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

void expectNoUseAfterFree(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("refcount over-release") == std::string::npos);
}

// The issue's own program: the getter (`using var z` keeps it outside the
// #8767 confinement proof) and its method twin are the same body.
std::string program(const std::string& readExpr) {
    return std::string(
               "module P; "
               "import System; "
               "class Cell "
               "  public static n: int := 0; "
               "  public v: int; "
               "  public constructor(x: int) this.v := x; Cell.n := Cell.n + 1; end constructor; "
               "  destructor Cell.n := Cell.n - 1; end destructor; "
               "end Cell; "
               "class H "
               "  public property V: int "
               "    get => do "
               "      using var z := new Cell(0); "
               "      var t := new Cell(1); "
               "      var u := move t; "
               "      t := new Cell(2); "
               "      return u.v + t.v; "
               "    end; "
               "  end V; "
               "  public method M(): int "
               "    using var z := new Cell(0); "
               "    var t := new Cell(1); "
               "    var u := move t; "
               "    t := new Cell(2); "
               "    return u.v + t.v; "
               "  end M; "
               "end H; "
               "public class Program "
               "  public static method Main(): int "
               "    var h := new H(); "
               "    var acc := 0; "
               "    for var i in 0..<200 do acc := acc + ") +
           readExpr +
           "; end; "
           "    Console.WriteLine(acc); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// THE REGRESSION ROW: the issue's repro, 200 reads of the getter. RED on the
// base (heap-use-after-free in `bz$prop$V$get`), GREEN with the fix: every
// read sums 1 + 2.
BEZEL_TEST(getter_unconfined_moved_from_reassign_8944,
           getter_block_body_reassigning_a_moved_from_local_frees_nothing_200x) {
    expectNoUseAfterFree(runAotCheckedLenient(program("h.V")), "600\n");
}

// The same getter read through `?.` (a separate read lowering of the inlined
// getter).
BEZEL_TEST(getter_unconfined_moved_from_reassign_8944,
           getter_nullcond_read_of_the_same_body_200x) {
    expectNoUseAfterFree(runAotCheckedLenient(program("h?.V!")), "600\n");
}
