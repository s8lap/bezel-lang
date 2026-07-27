// #1921 case A / §27.5 / §28.9: a closure LITERAL (`lambda () => ...`) or
// method reference passed DIRECTLY as a call argument to a PLAIN (non-iterator,
// non-async) user method is a +1 TEMPORARY (the producer's reference). The
// callee uses the closure synchronously during the call and never retains it
// past the call boundary (a plain method has no frame capture). The producer's
// transient reference must therefore be released ONCE the call returns —
// otherwise the 40-byte closure cell leaks (LSan: 40 bytes per call, the #1864
// sibling restricted to sync iterators did not cover this case).
//
// §28.9 / §27.5 / §28.3 design:
//   - The closure cell's rc starts at 1 (the producer's +1; see lowerLambda).
//   - The callee invokes the closure as a parameter borrow (no extra retain —
//     params are borrows by default).
//   - On return, the callee's stack reference is gone. Releasing the caller's
//     +1 brings rc to 0 → bz_closure_release reclaims the cell + env block.
//   - Safe direction: if the callee STORES the closure in a FIELD, the field-
//     store path (lowerClosureFieldStore) TRANSFERS the producer's +1 (no extra
//     retain) — the field becomes the sole owner; our release is the design
//     intent, NOT a UAF.
//   - UNSAFE shapes that are EXCLUDED from this release:
//       * async iterators (frame is suspended across yields and does not retain
//         at the call boundary),
//       * closure-RETURNING methods (the return value borrows the closure and
//         would dangle on release).
//   - Both exclusions are gated on `!isAsyncMethod(ms)` and the declared return
//     type NOT being a Function (Function-kind TypeRef).
//
// Spec citations: §27.5 (closure values), §28.3 (producers / transfers), §28.9
// (runtime-managed values + reclaim with instance), §28.4 (dispose-exactly-once).
//
// Test matrix:
//   1. Plain non-iterator method invoked with a closure-typed argument: the
//      callee consumes the closure synchronously (calls it) — the temporary
//      cell must be reclaimed (LSan-clean under runAotChecked).
//   2. Closure passed across a TRAILING STATEMENT (the #1864 lesson: a trailing
//      statement surfaces the otherwise-masked leak by clobbering the dead
//      register that still held the closure pointer).
//   3. The callee's returned VALUE is the canonical correctness oracle (R.Id
//      proves the closure was actually invoked and produced the right value).
//
// Pre-fix: LSan reports "Direct leak of 40 byte(s) ... calloc ... in main".
// Post-fix: LSan clean (no leak), exit code correct, no UAF/double-free.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent: runAotChecked returns ran=false with a "SKIP" diagnostic. clang is
// required to build this repo, so a skip is not expected — guard anyway.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// Count non-overlapping occurrences of `needle` in `hay`.
int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// The exact #1921 case A repro: a plain (non-iterator, non-async) user method
// `Use(make: () -> owned R)` that synchronously consumes the closure via a
// single `make()` call. Caller passes `lambda () => new R()` as the argument.
// The closure cell's +1 must be released after the call (it is consumed
// synchronously; no callee-side retention).
//
// Trailing statement (Console.WriteLine) follows the #1864 lesson: a dead-
// pointer register clobber can mask a closure leak in steady state, so a
// trailing statement surfaces it. The R destructor prints "D" once per `new R()`
// invoked inside `make()` (via `make()` at the call site).
const char* kPlainMethodConsumesClosure =
    "module M; "
    "class R "
    "  public Id: int := 7; "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"D\"); end destructor; "
    "end R; "
    "class C "
    "  public static method Use(make: () -> owned R): int "
    "    using var r := make(); "
    "    return r.Id; "
    "  end Use; "
    "  public static method Main(): int "
    "    var sum := C.Use(lambda () => new R()); "
    "    Console.WriteLine(\"sum=\" + sum.ToString()); "  // trailing stmt — surfaces the leak
    "    return sum; "
    "  end Main; "
    "end C;";

// Variant: closure consumed inside a LOOP. Each iteration the callee invokes
// `make()` once and the result is disposed by the using-var; the closure cell
// must be reclaimed each call (no LSan accumulation). The closure cell leaks
// pre-fix (40 bytes × N iterations); LSan-clean post-fix.
const char* kLoopOfPlainMethodCalls =
    "module M; "
    "class R "
    "  public Id: int := 0; "
    "  public constructor() Id := 1; end constructor; "
    "  destructor Console.WriteLine(\"D\"); end destructor; "
    "end R; "
    "class C "
    "  public static method Use(make: () -> owned R): int "
    "    using var r := make(); "
    "    return r.Id; "
    "  end Use; "
    "  public static method Main(): int "
    "    var s := 0; "
    "    var i := 0; "
    "    while i < 50 do "
    "      s := s + C.Use(lambda () => new R()); "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(\"s=\" + s.ToString()); "
    "    return s; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #1921 case A — single plain-method call: closure cell is reclaimed.
// runAotChecked = ASan/LSan oracle; a leaked cell flips r.ran to false.
// =====================================================================

BEZEL_TEST(closure_arg_plain_method_release_1921,
           single_call_closure_cell_reclaimed_spec_28_9) {
    JitResult r = runAotChecked(kPlainMethodConsumesClosure);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);             // compiled + LSan-clean (no 40-byte leak)
    BEZEL_EXPECT_EQ(r.exitCode, 7);       // r.Id == 7
    // The single `new R()` (via make() inside Use) was disposed by `using var r`
    // exactly once; double-free would also flip r.ran via ASan abort.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D"), 1);
}

BEZEL_TEST(closure_arg_plain_method_release_1921,
           loop_closure_cell_reclaimed_each_iter_spec_28_9) {
    JitResult r = runAotChecked(kLoopOfPlainMethodCalls);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);             // compiled + LSan-clean across 50 iterations
    BEZEL_EXPECT_EQ(r.exitCode, 50);      // 50 × 1 (each make() returns Id 1)
    // 50 `new R()` calls — each disposed exactly once. ASan would abort on a
    // double-free; LSan would flag a missing disposal. Both must be exactly 50.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D"), 50);
}
