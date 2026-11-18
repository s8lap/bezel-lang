// Codegen — issue #8747: a closure local captured by a second, local lambda
// and also passed to a store that keeps the pointer without a reference
// (`fs.Add(g)`, #5866) was freed by the capturing lambda's environment
// destructor, so the caller's collection held a freed closure.
//
// THE RULE. §28.9 (v0.359, "Runtime-managed types"): a function value's
// lifetime is managed by the runtime without user involvement, so a function
// value a collection still holds stays callable. §27.5 (v0.359): a closure's
// captures live as long as the closure. (§28.9 leaves the lifecycle mechanism
// to the implementation; the citation is for the observable guarantee.)
//
// ROOT CAUSE. The first capture of `g` moves the local's reference into the
// capture cell, and the capturing lambda's envdtor (`bz$lam$N$envdtor`)
// releases the cell's content when that lambda dies at `Fill`'s scope end.
// `fs.Add(g)` takes no reference, so the envdtor's release was the last one
// while `fs` still held the pointer. MEASURED on b279e8daf: heap-use-after-
// free in `main`, freed by `bz_closure_release` from the envdtor.
//
// THE FIX. #8723's donor pass (`collectClosureReleaseEligible`) already
// finds the local whose reference a kept store depends on: here `g` itself,
// or `g` behind `b := g;` / `var cmp := g;`. When that donor is captured, it
// is recorded in `closureKeptCaptureLocals_`, and `lowerLambda` suppresses
// both the capture's retain and the envdtor's release of the cell content
// for it (`suppressContentRcInEnvdtor`, the #4198 trade for a capture whose
// reference another consumer claims). The stored value then leaks, as a bare
// `fs.Add(g)` does, so the oracle is `runAotCheckedLenient` (AddressSanitizer
// on, LeakSanitizer off) with the full stdout compared. The control runs
// under `runAotChecked` (both on).
//
// Every program loops 200 times and runs under generated-code
// AddressSanitizer.
//
// MEASURED (`bezelc --sanitize=address --link=system`, detect_leaks=1):
//   row                                              b279e8daf      #8723 only     with the fix
//   issue_repro_capture_then_add_spec_28_9           UAF, 0 lines   UAF, 0 lines   leak, 201 lines
//   add_then_capture_spec_28_9                       UAF, 0 lines   UAF, 0 lines   leak, 201 lines
//   captured_twice_then_add_spec_27_5                UAF, 0 lines   UAF, 0 lines   leak, 201 lines
//   capture_then_alias_assign_add_spec_28_9          UAF, 0 lines   UAF, 0 lines   leak, 201 lines
//   capture_then_owned_alias_add_spec_28_9           UAF, 0 lines   UAF, 0 lines   leak, 201 lines
//   control_capture_without_store_stays_released...  clean          clean          clean
// (UAF = heap-use-after-free in main when the stored closure is called.)

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The program's stdout when it compiled, ran to exit code 0 and no memory
// error fired (AddressSanitizer on, LeakSanitizer off); a marker otherwise.
std::string runNoMemoryErrorOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// Iteration i calls the closure `x - y + i` twice: (1 - 2 + i) + (4 - 1 + i).
std::string expected200() {
    std::string e;
    for (int i = 0; i < 200; ++i) e += std::to_string(2 + 2 * i) + "\n";
    e += "DONE\n";
    return e;
}

// `fillBody` is the body of `Fill(fs, k)` after `g`'s declaration; it must
// leave exactly one closure, `g`'s, in `fs`.
std::string fillProgram(const std::string& fillBody) {
    return R"BZL(
module M;
public class C
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
)BZL" + fillBody + R"BZL(
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      Fill(fs, i);
      Console.WriteLine(fs[0](1, 2) + fs[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL";
}

}  // namespace

// #8747's repro, as filed.
BEZEL_TEST(closure_local_captured_kept_store_8747,
           issue_repro_capture_then_add_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    var l: (int) -> int := lambda(z: int) => g(z, 1);\n"
        "    var t := l(3);\n"
        "    fs.Add(g);\n")),
        expected200());
}

// The store comes first and the capture after it.
BEZEL_TEST(closure_local_captured_kept_store_8747,
           add_then_capture_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    fs.Add(g);\n"
        "    var l: (int) -> int := lambda(z: int) => g(z, 1);\n"
        "    var t := l(3);\n")),
        expected200());
}

// Two lambdas share `g`'s cell; neither envdtor may release the content.
BEZEL_TEST(closure_local_captured_kept_store_8747,
           captured_twice_then_add_spec_27_5) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    var l: (int) -> int := lambda(z: int) => g(z, 1);\n"
        "    var l2: (int) -> int := lambda(z: int) => g(1, z);\n"
        "    var t := l(3) + l2(4);\n"
        "    fs.Add(g);\n")),
        expected200());
}

// Capture plus #8723's alias assignment: `g` is the donor behind `b`.
BEZEL_TEST(closure_local_captured_kept_store_8747,
           capture_then_alias_assign_add_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    var l: (int) -> int := lambda(z: int) => g(z, 1);\n"
        "    var t := l(3);\n"
        "    var b: (int, int) -> int := lambda(x: int, y: int) => 0;\n"
        "    b := g;\n"
        "    fs.Add(b);\n")),
        expected200());
}

// Capture plus an owned alias declaration (`var cmp := g;`).
BEZEL_TEST(closure_local_captured_kept_store_8747,
           capture_then_owned_alias_add_spec_28_9) {
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(fillProgram(
        "    var l: (int) -> int := lambda(z: int) => g(z, 1);\n"
        "    var t := l(3);\n"
        "    var cmp: (int, int) -> int := g;\n"
        "    fs.Add(cmp);\n")),
        expected200());
}

// BASE-GREEN CONTROL (lane contract §7; green on b279e8daf and with the
// fix): `g` is captured and called but never stored, so it is no donor, the
// envdtor still releases the cell's content, and nothing leaks. It pins that
// the suppression is confined to donors: a fix that suppressed the envdtor
// release for every captured closure local would leak `g`'s closure here. Run(i) =
// (3 - 1 + i) + (1 - 2 + i) = 1 + 2i; summed over 0..199: 40000.
BEZEL_TEST(closure_local_captured_kept_store_8747,
           control_capture_without_store_stays_released_spec_27_5) {
    auto r = runAotChecked(R"BZL(
module M;
public class C
  public static method Run(k: int): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var l: (int) -> int := lambda(z: int) => g(z, 1) + k;
    return l(3) + g(1, 2) + k;
  end Run;
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do
      s := s + Run(i);
    end for;
    Console.WriteLine(s);
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("40000\nDONE\n"));
}
