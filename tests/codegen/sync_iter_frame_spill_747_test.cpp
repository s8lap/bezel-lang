// Codegen — a for-loop inside a SYNC iterator (`yield return`) must spill its
// implicit locals (index, loop variable, enumerator) into the state-machine
// frame (§29.3, #747 R42).
//
// `for var x in <Array>` inside a sync iterator emitted "Instruction does not
// dominate all uses" (IR_FAIL): the array-ptr / length SSA temporaries and the
// index/var allocas are computed in state 0 but used after a resume from a
// later state. The sync MoveNext now carries a spill-area field (like the async
// resume) and runs fixupSuspendDominance when a suspending for-loop registers
// its implicit allocas, so they survive across the yield.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// for-over-Array inside a sync iterator — was IR_FAIL; now yields all elements.
BEZEL_TEST(codegen_sync_iter_frame_spill_747, for_over_array_yields_all) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Echo(items: Array<int>): IEnumerable<int> "
        "    for var x in items do yield return x; end; "
        "  end Echo; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(10); a.Add(20); a.Add(30); "
        "    var sum := 0; "
        "    for var v in Echo(a) do sum := sum + v; end; "
        "    return sum; "
        "  end Main; end C;"), 60);
}

// for-over-IEnumerable param inside a sync iterator.
BEZEL_TEST(codegen_sync_iter_frame_spill_747, for_over_ienumerable_yields_all) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Echo(items: IEnumerable<int>): IEnumerable<int> "
        "    for var x in items do yield return x; end; "
        "  end Echo; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(10); a.Add(20); a.Add(30); "
        "    var sum := 0; "
        "    for var v in Echo(a) do sum := sum + v; end; "
        "    return sum; "
        "  end Main; end C;"), 60);
}

// The loop VARIABLE must survive the yield: read `x` after a yield. For {1,2}:
// yields 1,10,2,20 → 33.
BEZEL_TEST(codegen_sync_iter_frame_spill_747, loop_var_survives_yield) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Echo(items: Array<int>): IEnumerable<int> "
        "    for var x in items do yield return x; yield return x * 10; end; "
        "  end Echo; "
        "  public static method Main(): int "
        "    using var a := new Array<int>(); a.Add(1); a.Add(2); "
        "    var sum := 0; "
        "    for var v in Echo(a) do sum := sum + v; end; "
        "    return sum; "
        "  end Main; end C;"), 33);
}

// CONTROL (must not regress): a range-for sync iterator already worked (its
// single slot is frame-backed); the new spill must leave it unchanged.
BEZEL_TEST(codegen_sync_iter_frame_spill_747, range_for_sync_iter_unregressed) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Squares(): IEnumerable<int> "
        "    for var i in 1..3 do yield return i * i; end; "
        "  end Squares; "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var v in Squares() do sum := sum + v; end; "
        "    return sum; "  // 1+4+9
        "  end Main; end C;"), 14);
}
