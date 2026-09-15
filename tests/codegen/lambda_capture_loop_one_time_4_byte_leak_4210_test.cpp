// Spec-derived regression test for fix #4210 —
// `Lambda capturing an outer local + new allocation inside a loop leaks a
// one-time 4-byte allocation`.
//
// THE DEFECT. A lambda created inside a loop that (a) captures an outer local
// and (b) allocates `new SomeClass(capturedLocal)` leaked exactly ONE 4-byte
// allocation (one cell allocation for the captured local), from `calloc`
// directly in `main`. The leak was one-time per process regardless of loop
// trip count.
//
// ROOT CAUSE (per §27.5 / §28.9 / #4210). The second (shared-cell) path in
// `collectCellFreeEligible`'s lowerLambda emission checked
// `isSharedCellEnvdtorFreeOwner(key, &lam)`, which requires the lambda to be
// in `sharedCellEnvdtorFreeOwner_`. That map is populated ONLY for escaping
// lambdas (the populating loop at Setup.cpp:33638 requires
// `definitelyEscapingLambdas_.count(owner)` — condition (b)). For a
// non-escaping lambda in a loop, `sharedCellEnvdtorFreeOwner_` is always
// empty, so `isSharedCellEnvdtorFreeOwner` always returns false, and the
// second path never fires. Combined with the first path requiring the lambda
// to definitely escape (fails for non-escaping lambdas), `ce.freeCellInEnvdtor`
// was never set for non-escaping lambdas. The capture cell was allocated but
// never freed.
//
// THE FIX. Replace `isSharedCellEnvdtorFreeOwner(key, &lam)` with a direct
// `singleCaptureLocals_.count(key)` check. For a non-escaping lambda captured
// by exactly one env (the singleCaptureLocals_ proof), the env is the sole
// owner of the cell and outlives the lambda body, so freeing the cell in the
// envdtor is safe regardless of loop nesting. The `loopStack_.empty()`
// guard is also removed from this path since `singleCaptureLocals_`'s
// single-owner proof already covers the backward-edge concern.
//
// MEASUREMENT (ASan/LSan, r18-c8-lane-17, 93e5934):
//   FAIL-WITHOUT: "Direct leak of 4 byte(s) in 1 object(s)"
//   PASS-WITH:    zero leaks

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

}  // namespace

// per spec §27.5 / §28.9 / #4210: the verbatim reproducer from the issue.
// ASan reports a 4-byte leak from `calloc` in `main` for the captured local's
// box cell. The lambda is NON-escaping (called immediately, no escape path),
// created inside a loop, and captures an outer `int` local.
BEZEL_TEST(lambda_capture_loop_one_time_4_byte_leak_4210, verbatim_reproducer) {
    auto r = runAotChecked(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var f := lambda(): int => do "
        "        var x := new Res(i); "
        "        x.Dispose(); "
        "        return 1; "
        "      end; "
        "      _ := f(); "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // no leaks
}

// per spec §27.5 / §28.9 / #4210: same pattern without loop — non-escaping
// lambda captures outer local. The cell should be freed in the envdtor.
BEZEL_TEST(lambda_capture_loop_one_time_4_byte_leak_4210, no_loop_non_escaping_capture) {
    auto r = runAotChecked(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var f := lambda(): int => do "
        "      var x := new Res(i); "
        "      x.Dispose(); "
        "      return 1; "
        "    end; "
        "    _ := f(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// per spec §27.5 / §28.9 / #4210: two lambdas in sequence, same captured local.
// The second lambda reuses the already-boxed cell (alreadyBoxed path). Each
// envdtor should free the cell it owns. No leaks.
BEZEL_TEST(lambda_capture_loop_one_time_4_byte_leak_4210, two_sequential_non_escaping) {
    auto r = runAotChecked(
        "module M; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var f1 := lambda(): int => do var x := new Res(i); x.Dispose(); return 1; end; "
        "    _ := f1(); "
        "    var f2 := lambda(): int => do var y := new Res(i); y.Dispose(); return 2; end; "
        "    _ := f2(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// per spec §27.5 / §28.9 / #4210: control — an INT captured local (value type,
// 4 bytes) should not leak. Also verify the lambda body executes correctly.
BEZEL_TEST(lambda_capture_loop_one_time_4_byte_leak_4210, int_capture_returns_correct_value) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    var i := 0; "
        "    while i < 10 do "
        "      var f := lambda(): int => i; "
        "      sum := sum + f(); "
        "      i := i + 1; "
        "    end; "
        "    return sum; "  // 0+1+2+...+9 = 45
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 45);
}
