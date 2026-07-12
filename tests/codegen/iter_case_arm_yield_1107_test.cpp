// Regression tests for #1107 / §29.3 (sweep #9, findings #16 + #17): a
// `yield return` inside a `case`-statement arm.
//
// Three yield-detection walkers all failed to recurse into `case` arms:
//   - sema `stmtsContainYield` → an iterator whose whole body is a yielding
//     `case` was misclassified as a normal value-returning method and wrongly
//     rejected "does not return a value on every path" (§21.7).  [#16 BAD_REJECT]
//   - codegen `containsYield` → the method was lowered as a normal function.
//   - codegen `collectYields` (the resume-state-block counter) → the arm yields
//     were uncounted, so the yield-return lowering's
//     `nextState < smStateBBs_.size()` guard failed, the insert point stayed on
//     the already-`ret`'d arm block, and the case lowering's branch was appended
//     after the terminator → "Terminator found in the middle of a basic block"
//     IR-verification crash (code after the case, or any case-arm yield).  [#17]
//
// All three now recurse into block-form `case` arms in arm order. Found by
// adversarial discovery sweep #9.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1107/#16: an iterator whose entire body is a `case` whose every arm yields
// compiles (was rejected §21.7) and produces the yielded values.
BEZEL_TEST(iter_case_arm_yield_1107, case_bodied_iterator_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class T "
        "  public static method Gen(n: int): IEnumerable<int> "
        "    case n of "
        "      1 => do yield return 10; end, "
        "      2 => do yield return 20; end, "
        "      _ => do yield return 0; end "
        "    end; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in T.Gen(1) do s := s + x; end; "
        "    for var x in T.Gen(2) do s := s + x; end; "
        "    return s; "                                    // 10 + 20 = 30
        "  end Main; end T;"), 30);
}

// #1107/#17: a `yield return` AFTER a `case` (which itself yields in its arms)
// compiles (was an IR-verification crash) and yields both.
BEZEL_TEST(iter_case_arm_yield_1107, code_after_case_yields) {
    BEZEL_EXPECT_EQ(run(
        "module M; class T "
        "  public static method Gen(): IEnumerable<int> "
        "    var x := 1; "
        "    case x of "
        "      1 => do yield return 10; end, "
        "      _ => do yield return 0; end "
        "    end; "
        "    yield return 99; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var v in T.Gen() do s := s + v; end; "
        "    return s; "                                    // 10 + 99 = 109
        "  end Main; end T;"), 109);
}

// Multiple yields in a single arm consume distinct resume slots.
BEZEL_TEST(iter_case_arm_yield_1107, multiple_yields_in_arm) {
    BEZEL_EXPECT_EQ(run(
        "module M; class T "
        "  public static method Gen(): IEnumerable<int> "
        "    case 1 of "
        "      1 => do yield return 1; yield return 2; yield return 3; end, "
        "      _ => do yield return 0; end "
        "    end; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s := 0; for var x in T.Gen() do s := s + x; end; return s; "  // 6
        "  end Main; end T;"), 6);
}

// `yield break` inside a case arm terminates the iterator (trailing yield not
// produced).
BEZEL_TEST(iter_case_arm_yield_1107, yield_break_in_arm) {
    BEZEL_EXPECT_EQ(run(
        "module M; class T "
        "  public static method Gen(n: int): IEnumerable<int> "
        "    case n of "
        "      1 => do yield return 5; yield break; end, "
        "      _ => do yield return 9; end "
        "    end; "
        "    yield return 100; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s := 0; for var x in T.Gen(1) do s := s + x; end; return s; "  // 5
        "  end Main; end T;"), 5);
}

// Code before AND after the case.
BEZEL_TEST(iter_case_arm_yield_1107, code_before_and_after_case) {
    BEZEL_EXPECT_EQ(run(
        "module M; class T "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 1; "
        "    case 2 of "
        "      2 => do yield return 2; end, "
        "      _ => do yield return 0; end "
        "    end; "
        "    yield return 3; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s := 0; for var x in T.Gen() do s := s + x; end; return s; "  // 6
        "  end Main; end T;"), 6);
}

// Regression guard: a plain multi-yield iterator (no case) is unchanged.
BEZEL_TEST(iter_case_arm_yield_1107, plain_iterator_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class T "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 1; yield return 2; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s := 0; for var x in T.Gen() do s := s + x; end; return s; "  // 3
        "  end Main; end T;"), 3);
}
