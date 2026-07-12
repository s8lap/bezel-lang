// Codegen fix #587 / §20.5 — `++` / `--` on a user-defined indexer slot and a
// computed property with a block setter must call the setter to write the
// incremented/decremented value back.
//
// Root cause: the IncrementStmt handler called lvalueAddr which returns null for
// user-defined indexers (no backing LLVM ArrayType) and for computed properties
// (no backing alloca). After null, the handler silently did nothing, so
// `v[0]++` and `obj.P++` had no effect.
//
// Fix (LlvmBackend_Stmts.cpp, IncrementStmt branch): after lvalueAddr returns
// null, two new paths handle write-back:
//   (a) Block-setter computed property (`obj.P++`): read via getter, compute ±1,
//       write via setter — mirrors lowerPropertyCompound (§22.9).
//   (b) User-defined indexer (`v[i]++`): evaluate receiver and index once, call
//       getter, compute ±1, call setter — mirrors lowerIndexerCompound (§20/§23.4).
//
// Spec: §20.5 (increment/decrement on property/indexer lvalues), §22.9 (property
// setters), §23.4 (compound assignment desugars to getter + op + setter).

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
// §20.5 / §22.10: `++` on a user-defined indexer — setter must be called
// ---------------------------------------------------------------------------

// Primary repro from issue #587: v[0]++ must store the incremented value via
// the setter; a subsequent getter read must return 1, not 0.
BEZEL_TEST(indexer_incdec_587, indexer_postfix_increment_calls_setter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class IntVec "
        "  private _data: [4]int; "
        "  public property [i: int]: int "
        "    get => _data[i]; "
        "    set => do _data[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var v := new IntVec(); "
        "    v[0]++; "
        "    return v[0]; "  // expected 1
        "  end Main; "
        "end IntVec;"), 1);
}

// §20.5: `--` on a user-defined indexer — setter must store decremented value.
BEZEL_TEST(indexer_incdec_587, indexer_postfix_decrement_calls_setter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class IntVec "
        "  private _data: [4]int; "
        "  public property [i: int]: int "
        "    get => _data[i]; "
        "    set => do _data[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var v := new IntVec(); "
        "    v[0] := 5; "
        "    v[0]--; "
        "    return v[0]; "  // expected 4
        "  end Main; "
        "end IntVec;"), 4);
}

// §20.5: multiple increments at the same index slot accumulate correctly.
BEZEL_TEST(indexer_incdec_587, indexer_increment_twice) {
    BEZEL_EXPECT_EQ(run(
        "module M; class IntVec "
        "  private _data: [4]int; "
        "  public property [i: int]: int "
        "    get => _data[i]; "
        "    set => do _data[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var v := new IntVec(); "
        "    v[0]++; "
        "    v[0]++; "
        "    return v[0]; "  // expected 2
        "  end Main; "
        "end IntVec;"), 2);
}

// §20.5 / §23.4: increment at a non-zero index must update that slot only.
BEZEL_TEST(indexer_incdec_587, indexer_increment_nonzero_index) {
    BEZEL_EXPECT_EQ(run(
        "module M; class IntVec "
        "  private _data: [4]int; "
        "  public property [i: int]: int "
        "    get => _data[i]; "
        "    set => do _data[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var v := new IntVec(); "
        "    v[0] := 10; "
        "    v[1] := 20; "
        "    v[1]++; "
        "    return v[0] + v[1]; "  // 10 + 21 = 31
        "  end Main; "
        "end IntVec;"), 31);
}

// §20.5: `++` inside a loop via user-defined indexer — counter accumulates.
BEZEL_TEST(indexer_incdec_587, indexer_increment_in_loop) {
    BEZEL_EXPECT_EQ(run(
        "module M; class IntVec "
        "  private _data: [4]int; "
        "  public property [i: int]: int "
        "    get => _data[i]; "
        "    set => do _data[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var v := new IntVec(); "
        "    var n := 0; "
        "    while n < 5 do "
        "      v[0]++; "
        "      n++; "
        "    end; "
        "    return v[0]; "  // expected 5
        "  end Main; "
        "end IntVec;"), 5);
}

// ---------------------------------------------------------------------------
// §20.5 / §22.9: `++` / `--` on a computed property with a block setter
// ---------------------------------------------------------------------------

// §20.5 / §22.9: `obj.P++` where P has get/set — setter must store the
// incremented value.
BEZEL_TEST(indexer_incdec_587, property_postfix_increment_calls_setter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int get => _x; set => do _x := value; end; end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c.P := 3; "
        "    c.P++; "
        "    return c.P; "  // expected 4
        "  end Main; "
        "end C;"), 4);
}

// §20.5 / §22.9: `obj.P--` where P has get/set — setter must store decremented.
BEZEL_TEST(indexer_incdec_587, property_postfix_decrement_calls_setter) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public _x: int := 0; "
        "  public property P: int get => _x; set => do _x := value; end; end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c.P := 10; "
        "    c.P--; "
        "    return c.P; "  // expected 9
        "  end Main; "
        "end C;"), 9);
}

// ---------------------------------------------------------------------------
// §20.5: regression — compound assignment (`+=`) on user indexer still works
// ---------------------------------------------------------------------------

// Regression guard: `v[0] += 10` must still work (was already correct;
// this confirms that the #587 fix does not disturb the compound-assign path).
BEZEL_TEST(indexer_incdec_587, compound_assign_indexer_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class IntVec "
        "  private _data: [4]int; "
        "  public property [i: int]: int "
        "    get => _data[i]; "
        "    set => do _data[i] := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var v := new IntVec(); "
        "    v[0] += 10; "
        "    return v[0]; "  // expected 10
        "  end Main; "
        "end IntVec;"), 10);
}

// Regression guard: `++` on a plain local int must still work.
BEZEL_TEST(indexer_incdec_587, local_int_increment_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var x := 3; x++; return x; "
        "  end Main; "
        "end C;"), 4);
}

// Regression guard: `--` on a plain local int must still work.
BEZEL_TEST(indexer_incdec_587, local_int_decrement_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var x := 7; x--; return x; "
        "  end Main; "
        "end C;"), 6);
}

// Regression guard: built-in inline array `d[0]++` must still work.
BEZEL_TEST(indexer_incdec_587, builtin_array_increment_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: [2]int; d[0] := 5; d[0]++; return d[0]; "
        "  end Main; "
        "end C;"), 6);
}

// Regression guard: auto-property `c.Value++` (backed by field, uses lvalueAddr)
// must still work.
BEZEL_TEST(indexer_incdec_587, auto_property_increment_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public property Value: int; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c.Value := 5; "
        "    c.Value++; "
        "    return c.Value; "  // expected 6
        "  end Main; "
        "end C;"), 6);
}
