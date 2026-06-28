// Codegen — OwnedDictionary.Drain() loop now fully implemented (#628 part 2).
//
// Issue #628 part 1 verified that the unimplemented drain loop emitted a hard
// error (no binary produced). Part 2 implements the feature; these tests are
// updated to verify the feature now COMPILES AND RUNS correctly.
//
// Spec references:
//   §4.9 v0.135 — OwnedDictionary.Drain() owned-consuming iteration

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runJit;

// ===========================================================================
// §4.9 / #628 part 2: drain loop must now compile and run (not fail).
// These were hard-error tests in part 1; they verify the opposite after part 2.
// ===========================================================================

// §4.9 / #628 part 2: drain over OwnedDictionary<string, int> — one entry
// added, loop increments count once, returns 1.
BEZEL_TEST(codegen_unimplemented_628, owned_dict_drain_for_is_hard_error) {
    JitResult r = runJit(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new OwnedDictionary<string, int>(); "
        "    d[\"a\"] := 1; "
        "    var count := 0; "
        "    for var entry in d.Drain() do "
        "      using var e := entry; "
        "      count := count + 1; "
        "    end; "
        "    return count; "
        "  end Main; "
        "end C;");
    // Part 2: drain is fully implemented — must compile and run.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // one entry drained
}

// §4.9 / #628 part 2: empty drain loop body compiles and runs (count stays 0).
BEZEL_TEST(codegen_unimplemented_628, owned_dict_drain_for_records_error) {
    JitResult r = runJit(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new OwnedDictionary<string, int>(); "
        "    for var entry in d.Drain() do "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    // Part 2: must compile and run cleanly.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §4.9 / #628 part 2: drain loop with `return` in body executes correctly.
// With the old bug the loop was silently dropped (returned 0). Now it must
// enter the loop body and return 1 (one entry was added).
BEZEL_TEST(codegen_unimplemented_628, owned_dict_drain_does_not_silently_succeed) {
    JitResult r = runJit(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new OwnedDictionary<string, int>(); "
        "    d[\"x\"] := 42; "
        "    for var entry in d.Drain() do "
        "      using var e := entry; "
        "      return 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    // Part 2: loop body executes — must return 1, not 0.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
