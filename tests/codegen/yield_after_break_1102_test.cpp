// Regression tests for #1102 part A (§29.3): a `yield return` that is
// UNREACHABLE because it follows a `yield break` still consumed a resume-state
// block in the MoveNext state machine, but the body flow stopped at the break,
// so that block was left with no terminator → "Basic Block %mn.sN does not have
// terminator!" IR-verification crash. Bezel allows unreachable code generally
// (`return 5; var x := 1;` compiles), so this is a codegen bug, not a sema
// reject. The fix terminates any orphan resume-state block with `ret false`
// (exhausted) — a resume into a post-break state cannot occur at runtime. Found
// by adversarial discovery sweep #8.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1102A: a `yield return` after a `yield break` compiles (was an IR crash); the
// dead yield is not produced.
BEZEL_TEST(yield_after_break_1102, yield_after_break_compiles) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 1; "
        "    yield break; "
        "    yield return 2; "                            // dead — never produced
        "  end Gen; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in C.Gen() do s := s + x; end; "
        "    return s; "                                  // only 1 yielded → 1
        "  end Main; end C;"), 1);
}

// Multiple dead yields after a break.
BEZEL_TEST(yield_after_break_1102, multiple_dead_yields) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 5; "
        "    yield break; "
        "    yield return 6; "
        "    yield return 7; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in C.Gen() do s := s + x; end; "
        "    return s; "                                  // 5
        "  end Main; end C;"), 5);
}

// Regression guard: a trailing `yield break` after real yields is unchanged.
BEZEL_TEST(yield_after_break_1102, trailing_break_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 10; "
        "    yield return 20; "
        "    yield break; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in C.Gen() do s := s + x; end; "
        "    return s; "                                  // 30
        "  end Main; end C;"), 30);
}

// Regression guard: a normal multi-yield iterator (no break) is unchanged.
BEZEL_TEST(yield_after_break_1102, normal_iterator_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 1; "
        "    yield return 2; "
        "    yield return 3; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in C.Gen() do s := s + x; end; "
        "    return s; "                                  // 6
        "  end Main; end C;"), 6);
}
