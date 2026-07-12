// Codegen — user-defined indexer compound assignment (`+=`, `-=`, `*=`, …)
// per §20 (indexers) and §23.4 (compound assignment desugars to get + op + set).
//
// BUG (silent wrong result): `a[i] += v` through a user-defined `property [i]:T`
// with get/set accessors computes the new value (getter + arithmetic) but never
// calls the setter — the write is silently dropped.
//
// Root cause: lowerStmt's compound-assign (`OP=`) branch had no IndexExpr path
// for user-defined indexers. It called lowerPropertyCompound (which skips
// isIndexer properties) and then fell through to lvalueAddr, which returns null
// for user-indexer targets (no backing LLVM ArrayType), so the whole operation
// became a no-op.
//
// Fix: in LlvmBackend_Assign.cpp, add lowerIndexerCompound which evaluates the
// receiver and index once, calls the getter, applies the operator, then calls
// the setter with the result.
//
// Tests are derived from §20 (indexers) and §23.4 (compound assignment).
// Controls verify that existing correct paths are undisturbed.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ---------------------------------------------------------------------------
// (a) Primary repro: user indexer `+=` must call the setter — per §23.4
// ---------------------------------------------------------------------------

// §20 / §23.4: `c[0] += 5` expands to `c[0] := c[0] + 5`; getter reads 10,
// result is 15, setter stores it; subsequent getter read must return 15.
BEZEL_TEST(indexer_compound_assign_20_23_4, plus_equals_calls_setter) {
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
        "    return c[0]; "
        "  end Main; "
        "end C;"), 15);
}

// ---------------------------------------------------------------------------
// (b) Other compound operators on a user indexer — per §23.4
// ---------------------------------------------------------------------------

// §23.4: `-=` through a user indexer must invoke the setter.
BEZEL_TEST(indexer_compound_assign_20_23_4, minus_equals_calls_setter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _d: [2]int; "
        "  public property [i: int]: int "
        "    get => _d[i]; "
        "    set => do _d[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c[0] := 20; "
        "    c[0] -= 8; "
        "    return c[0]; "
        "  end Main; "
        "end C;"), 12);
}

// §23.4: `*=` through a user indexer must invoke the setter.
BEZEL_TEST(indexer_compound_assign_20_23_4, times_equals_calls_setter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _d: [2]int; "
        "  public property [i: int]: int "
        "    get => _d[i]; "
        "    set => do _d[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c[0] := 6; "
        "    c[0] *= 7; "
        "    return c[0]; "
        "  end Main; "
        "end C;"), 42);
}

// §23.4: `/=` through a user indexer must invoke the setter.
BEZEL_TEST(indexer_compound_assign_20_23_4, divide_equals_calls_setter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _d: [2]int; "
        "  public property [i: int]: int "
        "    get => _d[i]; "
        "    set => do _d[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c[0] := 84; "
        "    c[0] /= 2; "
        "    return c[0]; "
        "  end Main; "
        "end C;"), 42);
}

// ---------------------------------------------------------------------------
// (c) User indexer with non-zero index — distinguishes per-slot storage
// ---------------------------------------------------------------------------

// §20 / §23.4: compound assign at index 1 must update slot 1, not slot 0.
BEZEL_TEST(indexer_compound_assign_20_23_4, compound_at_nonzero_index) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _d: [4]int; "
        "  public property [i: int]: int "
        "    get => _d[i]; "
        "    set => do _d[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c[1] := 3; "
        "    c[1] += 4; "   // slot 1 should become 7
        "    c[2] := 10; "
        "    c[2] *= 5; "   // slot 2 should become 50
        "    return c[1] + c[2]; "  // 7 + 50 = 57
        "  end Main; "
        "end C;"), 57);
}

// ---------------------------------------------------------------------------
// (d) User indexer backed by a different storage (field-based class)
// ---------------------------------------------------------------------------

// §20 / §23.4: a class that stores to a single int field (not an array);
// the indexer ignores the index and always reads/writes _value.
BEZEL_TEST(indexer_compound_assign_20_23_4, field_backed_indexer_compound) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _value: int; "
        "  public property [i: int]: int "
        "    get => _value; "
        "    set => do _value := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c[0] := 100; "
        "    c[0] += 23; "
        "    return c[0]; "
        "  end Main; "
        "end C;"), 123);
}

// ---------------------------------------------------------------------------
// (e) CONTROL: built-in array `d[0] += 5` still works correctly
// ---------------------------------------------------------------------------

// §15.1 / §23.4: compound assignment on a BUILT-IN inline array must not
// be disturbed by the user-indexer fix.
BEZEL_TEST(indexer_compound_assign_20_23_4, builtin_array_compound_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: [2]int; "
        "    d[0] := 10; "
        "    d[0] += 5; "
        "    return d[0]; "
        "  end Main; "
        "end C;"), 15);
}

// ---------------------------------------------------------------------------
// (f) CONTROL: plain user-indexer `:=` (set) still works correctly
// ---------------------------------------------------------------------------

// §20 / §22.10: a plain `c[0] := v` through a user-defined indexer setter must
// store the new value; this was working before and must stay correct.
BEZEL_TEST(indexer_compound_assign_20_23_4, plain_set_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _d: [2]int; "
        "  public property [i: int]: int "
        "    get => _d[i]; "
        "    set => do _d[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c[0] := 42; "
        "    return c[0]; "
        "  end Main; "
        "end C;"), 42);
}

// ---------------------------------------------------------------------------
// (g) CONTROL: plain user-indexer get still works correctly
// ---------------------------------------------------------------------------

// §20 / §22.10: `c[0]` (get only) must return the stored value unchanged.
BEZEL_TEST(indexer_compound_assign_20_23_4, plain_get_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _d: [2]int; "
        "  public property [i: int]: int "
        "    get => _d[i]; "
        "    set => do _d[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c[1] := 99; "
        "    return c[1]; "
        "  end Main; "
        "end C;"), 99);
}

// ---------------------------------------------------------------------------
// (h) CONTROL: `c[0] := c[0] + 5` (explicit read-modify-write) returns 15
// ---------------------------------------------------------------------------

// §23.4: the explicit expanded form must work as the baseline.
BEZEL_TEST(indexer_compound_assign_20_23_4, explicit_read_modify_write) {
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
        "    c[0] := c[0] + 5; "
        "    return c[0]; "
        "  end Main; "
        "end C;"), 15);
}

// ---------------------------------------------------------------------------
// (i) Chained compound assigns on different slots
// ---------------------------------------------------------------------------

// §20 / §23.4: multiple slots can be independently compound-assigned.
BEZEL_TEST(indexer_compound_assign_20_23_4, multiple_slots_compound) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  private _d: [4]int; "
        "  public property [i: int]: int "
        "    get => _d[i]; "
        "    set => do _d[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c[0] := 1; "
        "    c[1] := 2; "
        "    c[2] := 3; "
        "    c[0] += 10; "   // 11
        "    c[1] += 20; "   // 22
        "    c[2] += 30; "   // 33
        "    return c[0] + c[1] + c[2]; "  // 66
        "  end Main; "
        "end C;"), 66);
}
