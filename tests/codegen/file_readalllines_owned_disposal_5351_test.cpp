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
// THE TEST: FrozenArray<int>.From([...]) in a 501-iteration loop (`0..500` is
// a closed range, §16.3), verified clean under runAotChecked (ASan leak
// detection). Pre-fix: the loop leaks the FrozenArray headers. Post-fix:
// clean LSan.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

// runAotChecked: ASan-instrumented generated code +
// `ASAN_OPTIONS=detect_leaks=1:exitcode=99:abort_on_error=0`. A real
// sanitizer failure surfaces as `r.ran == false` (the LSan/ASan report
// arrives in `r.diagnostics`). A clean program returns normally with
// `r.ran == true`, its captured output in `r.stdoutText`, and the
// program's exit code in `r.exitCode`.
//
// #5351: FrozenArray<int>.From in a loop. Pre-fix: the FrozenArray headers
// leak (each From call produces an owned FrozenArray that was not
// disposed). Post-fix: clean LSan.
//
// The observable is the total element count, and it travels on captured
// stdout rather than in the exit status. Two reasons, both measured:
//
//   * The OS exit-status mechanism is 8 bits, so §33.3's "the returned value
//     becomes the process exit code" truncates silently. `0..500` is a CLOSED
//     range (§16.3 — `..` includes both endpoints), so the loop runs 501
//     times, not the 500 the inline comment assumed, and 501 × 5 = 2505.
//     Neither 2500 nor 2505 is representable: the program exits 201 (2505
//     mod 256), so the row's `== 2500` could never have passed. Asserting an
//     8-bit exit status is also a weak observation in its own right — a
//     wrong program can truncate to the same byte — so the value moves to
//     stdout, where no truncation is possible. This is the repo's standing
//     pattern for an observable above 255 (e.g. dict_string_key_leak_3703)
//     and it strengthens the row rather than relaxing it.
//   * `r.ran` stays the leak oracle. runAotChecked's `ran == false` is how
//     an LSan report surfaces, so the assertion keeps `BEZEL_EXPECT_TRUE`
//     on it and checks the value on stdoutText.
//
// `f.Length` is correct on this receiver: `FrozenArray<T>` declares
// `public readonly property Length: int` (stdlib §4.19) — it is a fixed
// extent, and §4.4's Count-vs-Length convention reserves `Count` for a
// resizable collection. Only `Array<T>` takes `Count`.
BEZEL_TEST(file_readalllines_owned_disposal_5351,
           frozen_array_from_500x_no_leak) {
    std::string prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..500 do "  // closed range: 501 iterations
        "      var f := FrozenArray<int>.From([1, 2, 3, 4, 5]); "
        "      total := total + f.Length; "
        "    end for; "
        "    Console.WriteLine($\"{total}\"); "  // 501 * 5 = 2505
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_TRUE(r.ran);  // ASan/LSan-clean: the loop leaked nothing
    BEZEL_EXPECT_EQ(r.stdoutText, "2505\n");
}

// Control: the same loop with `using var` should also be clean (this path
// was already working before the fix, confirming the harness itself is
// clean). Same stdout observation as the row above — same loop, same
// 501 iterations, same 2505.
BEZEL_TEST(file_readalllines_owned_disposal_5351,
           frozen_array_from_500x_using_var_control) {
    std::string prog =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..500 do "  // closed range: 501 iterations
        "      using var f := FrozenArray<int>.From([1, 2, 3, 4, 5]); "
        "      total := total + f.Length; "
        "    end for; "
        "    Console.WriteLine($\"{total}\"); "  // 501 * 5 = 2505
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "2505\n");
}
