// Codegen — compound assignment on a heap Array<T> indexer (`arr[i] OP= v`)
// per §17.15 (compound assignment desugars to lvalue = lvalue OP rhs) and
// §4.4 (Array<T> indexer get/set).
//
// BUG (#508): `arr[0] += 5` on a heap Array<int> (bz_array_t* backend) read
// the element via bz_array_get but silently dropped the set-back: the computed
// result was never stored via bz_array_set, so arr[0] stayed at the original
// value.
//
// Root cause: the compound-assignment branch in lowerStmt called
// lowerIndexerCompound (user-defined indexer) and lowerPropertyCompound
// (non-indexer computed property), then fell through to lvalueAddr. For a
// heap-collection IndexExpr lvalueAddr returns null (no LLVM ArrayType for a
// bz_array_t* receiver), so the whole compound assign was a silent no-op.
//
// Fix: add lowerCollectionIndexerCompound in LlvmBackend_Assign.cpp, called
// after lowerIndexerCompound; it evaluates receiver+index once, calls
// bz_array_get to read the current value, applies the operator, and calls
// bz_array_set (or bz_array_set_disposing for OwnedArray) to write back.
//
// Tests derived from §17.15 / §4.4. Controls verify that the pre-existing
// correct paths are undisturbed.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ---------------------------------------------------------------------------
// (a) Primary repro — §508 / §4.4 / §17.15
// ---------------------------------------------------------------------------

// §4.4 / §17.15: `arr[0] += 5` on a heap Array<int> must store 15, not 10.
// Before the fix: the set was silently dropped → arr[0] remained 10.
BEZEL_TEST(array_indexer_compound_508, plus_equals_stores_back_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); "
        "    arr[0] += 5; "
        "    return arr[0]; "  // want 15; was 10 before fix
        "  end Main; "
        "end C;"), 15);
}

// ---------------------------------------------------------------------------
// (b) Other compound operators on Array<int> — §4.4 / §17.15
// ---------------------------------------------------------------------------

// §17.15: `-=` must write back the decremented value.
BEZEL_TEST(array_indexer_compound_508, minus_equals_stores_back_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(30); "
        "    arr[0] -= 8; "
        "    return arr[0]; "  // 22
        "  end Main; "
        "end C;"), 22);
}

// §17.15: `*=` must write back the multiplied value.
BEZEL_TEST(array_indexer_compound_508, times_equals_stores_back_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(6); "
        "    arr[0] *= 7; "
        "    return arr[0]; "  // 42
        "  end Main; "
        "end C;"), 42);
}

// §17.15: `/=` must write back the divided value.
BEZEL_TEST(array_indexer_compound_508, divide_equals_stores_back_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(84); "
        "    arr[0] /= 2; "
        "    return arr[0]; "  // 42
        "  end Main; "
        "end C;"), 42);
}

// §17.15: `&=` (bitwise AND) must write back the result.
BEZEL_TEST(array_indexer_compound_508, and_equals_stores_back_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(15); "  // 0b00001111
        "    arr[0] &= 6; "  // 0b00000110 -> 6
        "    return arr[0]; "  // 6
        "  end Main; "
        "end C;"), 6);
}

// ---------------------------------------------------------------------------
// (c) Compound assign at a non-zero index — §4.4 / §17.15
// ---------------------------------------------------------------------------

// §4.4 / §17.15: compound assign at index 1 must update slot 1, not slot 0.
BEZEL_TEST(array_indexer_compound_508, nonzero_index_targets_correct_slot_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(100); "  // slot 0
        "    arr.Add(7);   "  // slot 1
        "    arr.Add(50);  "  // slot 2
        "    arr[1] += 3; "   // slot 1 becomes 10; others unchanged
        "    return arr[0] + arr[1] + arr[2]; "  // 100 + 10 + 50 = 160
        "  end Main; "
        "end C;"), 160);
}

// ---------------------------------------------------------------------------
// (d) Compound assign in a loop — §4.4 / §17.15
// ---------------------------------------------------------------------------

// §4.4 / §17.15: `for var i in 0..2 do arr[i] += i end` must update each
// slot independently.
BEZEL_TEST(array_indexer_compound_508, compound_in_loop_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); arr.Add(20); arr.Add(30); "
        "    for var i in 0..2 do "
        "      arr[i] += i; "
        "    end; "
        "    return arr[0] + arr[1] + arr[2]; "
        // arr[0]=10+0=10, arr[1]=20+1=21, arr[2]=30+2=32 → 63
        "  end Main; "
        "end C;"), 63);
}

// ---------------------------------------------------------------------------
// (e) Multiple compound assigns on different slots — §4.4 / §17.15
// ---------------------------------------------------------------------------

// §4.4 / §17.15: each slot is independently updated; no cross-slot aliasing.
BEZEL_TEST(array_indexer_compound_508, multiple_slots_compound_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); arr.Add(2); arr.Add(3); "
        "    arr[0] += 10; "  // 11
        "    arr[1] += 20; "  // 22
        "    arr[2] += 30; "  // 33
        "    return arr[0] + arr[1] + arr[2]; "  // 66
        "  end Main; "
        "end C;"), 66);
}

// ---------------------------------------------------------------------------
// (f) CONTROL: `arr[0] := arr[0] + 5` (explicit read-modify-write) — §4.4
// ---------------------------------------------------------------------------

// §4.4: the explicit expanded form was working before #508 and must stay correct.
BEZEL_TEST(array_indexer_compound_508, explicit_rmw_control_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); "
        "    arr[0] := arr[0] + 5; "
        "    return arr[0]; "  // 15
        "  end Main; "
        "end C;"), 15);
}

// ---------------------------------------------------------------------------
// (g) CONTROL: inline-array compound assign (`d[0] += 5`) still works — §15.1
// ---------------------------------------------------------------------------

// §15.1: the inline-array compound-assign path is distinct from the heap-
// collection path and must not be disturbed.
BEZEL_TEST(array_indexer_compound_508, inline_array_compound_unaffected_15_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: [2]int; "
        "    d[0] := 10; "
        "    d[0] += 5; "
        "    return d[0]; "  // 15
        "  end Main; "
        "end C;"), 15);
}

// ---------------------------------------------------------------------------
// (h) CONTROL: user-defined indexer `+=` still works — §20 / §23.4
// ---------------------------------------------------------------------------

// §20 / §23.4: the user-indexer compound path (lowerIndexerCompound) must not
// be disturbed — it is called before lowerCollectionIndexerCompound.
BEZEL_TEST(array_indexer_compound_508, user_indexer_compound_unaffected_20) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _d: [2]int; "
        "  public property [i: int]: int "
        "    get => _d[i]; "
        "    set => do _d[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c[0] := 10; "
        "    c[0] += 5; "
        "    return c[0]; "  // 15
        "  end Main; "
        "end C;"), 15);
}

// ---------------------------------------------------------------------------
// (i) CONTROL: Array<T> plain get/set still work after the fix — §4.4
// ---------------------------------------------------------------------------

// §4.4: plain `arr[i] := v` (non-compound) must still store correctly.
BEZEL_TEST(array_indexer_compound_508, plain_set_unaffected_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); arr.Add(2); "
        "    arr[0] := 99; "
        "    return arr[0] + arr[1]; "  // 101
        "  end Main; "
        "end C;"), 101);
}

// §4.4: plain `arr[i]` read must still return the stored value.
BEZEL_TEST(array_indexer_compound_508, plain_get_unaffected_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(42); "
        "    return arr[0]; "  // 42
        "  end Main; "
        "end C;"), 42);
}
