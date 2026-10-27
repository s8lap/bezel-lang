// Codegen / crash — a method with TWO `return lambda` statements that both
// capture the SAME LOCAL used to SEGV when the SECOND return was taken at
// runtime, because the captured local's BOX CELL was allocated inside the
// FIRST lambda's lowered capture site (a conditional branch) and the second
// lowerLambda's "already boxed" fast path skipped the guard and read the
// uninitialised entry-block zero. Spec §27.5 (closures), §28.9 (runtime-
// managed types). Spec revision v0.236.
//
// Repro shape (verbatim from issue #2303, `Make(1)` takes the second return):
//
//   public static method Make(n: int): () -> int
//     var m := n;
//     if m > 5 then
//       return lambda() => m;        // first returned lambda
//     end;
//     return lambda() => m + 100;    // second returned lambda, same captured local
//   end Make;
//
//   public static method Main(): int
//     var f := Make(1);              // takes the SECOND return
//     return f() - 94;               // want (1 + 100) - 94 = 7
//   end Main;
//
// THE BUG. `boxLocalSlot` (src/codegen/LlvmBackend_Layout.cpp) emits the
// cell allocation behind a null-check guard the FIRST time it is asked to
// box a slot. On a SUBSEQUENT boxLocalSlot call for the same slot (the
// second lambda's capture), the "already boxed" fast path returned the
// slot value directly without re-emitting the guard. That is sound only
// if the first alloc's basic block is always reached at runtime — which
// is not the case when the first lambda lives behind a conditional that
// the runtime did not take. The second lambda then stores the entry-block
// zero (the cell-holder's initial value) into its env, and the lambda body
// dereferences address 0 → SEGV.
//
// THE FIX. Emit the guard in the "already boxed" path too: null-check the
// cell-holder, and if it is still null (first alloc was skipped), allocate
// now from the original-slot sync entry. The cell is then guaranteed to
// exist before any lambda uses it, regardless of which capture path the
// runtime took.
//
// The mirror case (first return taken, `Make(7)`) already ran correctly on
// base; we pin it as the value-verify (returns 7, leaks the cell).
//
// OBSERVABILITY. `runAotChecked` (ASan/LSan on generated code). SEGV on
// base; clean exit 7 with the fix. The first-return path continues to run
// (value 7) — the fix is invisible there because the guard's first branch
// (non-null) is always taken after the first lambda's alloc.
//
// The negative sibling (capture-by-escaping only, single return) was already
// verified by `lambda_divergent_return_2107_test.cpp` and remains green.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotCheckedLenient;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

}  // namespace

// ===========================================================================
// A — second-return path: SEGV on base, exit 7 with the fix.
// Per spec §27.5 (closures) and §28.9 (runtime-managed types). v0.236.
// ===========================================================================
BEZEL_TEST(codegen_2303_two_return_lambda,
           second_return_no_segv_spec_27_5) {
    auto r = runAotCheckedLenient(
        "module App; "
        "public class Program "
        "  public static method Make(n: int): () -> int "
        "    var m := n; "
        "    if m > 5 then "
        "      return lambda() => m; "
        "    end; "
        "    return lambda() => m + 100; "
        "  end Make; "
        "  public static method Main(): int "
        "    var f := Make(1); "
        "    return f() - 94; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ===========================================================================
// B — first-return path (the issue's `Make(7)`): already green on base; we
// pin it so a future "while-we're-at-it" change cannot silently break the
// shape. The cell still leaks (the pre-existing #694 + #2334 residual; the
// 4-byte cell is a known contained leak — the issue explicitly notes the
// first-return path "leaks 4B but doesn't crash"). The exit code is the
// observable contract; the leak is the cell-bookkeeping residual that the
// fix makes sound (the second lambda's guard now allocates on demand).
// ===========================================================================
BEZEL_TEST(codegen_2303_two_return_lambda,
           first_return_still_clean_spec_27_5) {
    auto r = runAotCheckedLenient(
        "module App; "
        "public class Program "
        "  public static method Make(n: int): () -> int "
        "    var m := n; "
        "    if m > 5 then "
        "      return lambda() => m; "
        "    end; "
        "    return lambda() => m + 100; "
        "  end Make; "
        "  public static method Main(): int "
        "    var f := Make(7); "
        "    return f(); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ===========================================================================
// C — different local per return: each lambda captures a distinct local.
// No sharing, no re-entry of the "already boxed" path. Pin the negative
// axis so a future "merge all captures" regression cannot slip through.
// ===========================================================================
BEZEL_TEST(codegen_2303_two_return_lambda,
           distinct_locals_per_return_spec_27_5) {
    auto r = runAotCheckedLenient(
        "module App; "
        "public class Program "
        "  public static method Make(n: int): () -> int "
        "    var a := n; "
        "    var b := n + 1000; "
        "    if n > 5 then "
        "      return lambda() => a; "
        "    end; "
        "    return lambda() => b; "
        "  end Make; "
        "  public static method Main(): int "
        "    var f := Make(1); "
        "    return f() - 1001; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// D — second-return path through RECURSION (the original canary shape):
// `Make(n - 1)` from inside Make. The captured local `m` is shared between
// the outer Make's two return lambdas AND any inner Make call's lambdas.
// Same #2303 hazard: the second-return path's cell-holder may not have been
// allocated if the conditional was not taken on every recursion. With the
// fix the guard allocates on demand at every nested second-return, so the
// program runs clean. exit = ((2 - 1 + 100) - 94) = 7.
// ===========================================================================
BEZEL_TEST(codegen_2303_two_return_lambda,
           recursive_second_return_spec_27_5) {
    auto r = runAotCheckedLenient(
        "module App; "
        "public class Program "
        "  public static method Make(n: int): () -> int "
        "    var m := n; "
        "    if m > 5 then "
        "      return lambda() => m; "
        "    end; "
        "    if n > 1 then "
        "      return Make(n - 1); "
        "    end; "
        "    return lambda() => m + 100; "
        "  end Make; "
        "  public static method Main(): int "
        "    var f := Make(2); "
        "    return f() - 94; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
