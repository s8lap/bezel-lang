// Regression test for #5351 (§28.9, v0.313): File.ReadAllLines (and all
// builtin static factory calls) were classified as Borrowed by producerState
// even though their stdlib signatures declare `owned` returns. The local
// carried no disposal obligation and the result leaked silently.
//
// THE DEFECT: producerState returned Borrowed for these calls, so:
//   var lines := File.ReadAllLines(path);
// did not register scope-end disposal. The Array<string> and its string
// elements were leaked.
//
// THE FIX: producerState now returns Owned for calls registered in
// kOwnedStaticFactories (File.ReadAllLines, File.ReadAllBytes,
// Directory.GetFiles, Directory.GetDirectories, FrozenArray<T>.From,
// Stopwatch.StartNew, CancellationTokenSource factories). Codegen registers
// NO reclaim for these locals (only `using var` reaches the disposal arms),
// so the transfer path has exactly one disposer and admitting transfer
// cannot double-free.
//
// THE TEST: FrozenArray<int>.From([...]) in a 500-iteration loop, verified
// clean under runAotChecked (ASan leak detection). Pre-fix: the loop leaks
// the FrozenArray headers. Post-fix: clean LSan.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// runAotChecked: ASan-instrumented generated code +
// `ASAN_OPTIONS=detect_leaks=1:exitcode=99:abort_on_error=0`. A real
// sanitizer failure surfaces as `r.ran == false`. A clean program returns
// normally with `r.ran == true` and the program's exit code in `r.exitCode`.
int runChecked(const std::string& program) {
    JitResult r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// #5351: FrozenArray<int>.From in a 500-iteration loop. Pre-fix: the
// FrozenArray headers leak (each From call produces an owned FrozenArray
// that was not disposed). Post-fix: clean LSan.
// The loop amplifies the leak to make it detectable: 500 × ~32 B =
// ~16 KB leaked without the fix, vs 0 B with it.
BEZEL_TEST(file_readalllines_owned_disposal_5351,
           frozen_array_from_500x_no_leak) {
    std::string prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..500 do "
        "      var f := FrozenArray<int>.From([1, 2, 3, 4, 5]); "
        "      total := total + f.Length; "
        "    end for; "
        "    return total; "  // should be 500 * 5 = 2500
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 2500);
}

// Control: the same loop with `using var` should also be clean (this path
// was already working before the fix, confirming the harness itself is clean).
BEZEL_TEST(file_readalllines_owned_disposal_5351,
           frozen_array_from_500x_using_var_control) {
    std::string prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..500 do "
        "      using var f := FrozenArray<int>.From([1, 2, 3, 4, 5]); "
        "      total := total + f.Length; "
        "    end for; "
        "    return total; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 2500);
}
