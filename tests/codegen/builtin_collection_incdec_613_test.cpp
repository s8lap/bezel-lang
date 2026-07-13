// Codegen fix #613 / §25.1 / §4.4 / §4.8 — `arr[i]++` / `arr[i]--` and
// `dict[k]++` / `dict[k] += n` on built-in heap collections (Array<int>,
// Dictionary<string,int>) are a silent no-op: the element/value is read into
// a temp, the increment is computed, but the result is never written back via
// bz_array_set / bz_dict_set_str.
//
// Root cause (LlvmBackend_Stmts.cpp, IncrementStmt handler): after
// lvalueAddr returns null (no LLVM ArrayType for bz_array_t*), the #587 fix
// added two write-back paths:
//   (a) block-setter computed property — not applicable to Array<T>
//   (b) user-defined indexer (findIndexer) — not applicable; Array<T> and
//       Dictionary<K,V> are built-in and carry no PropertyDecl in their sema
//       TypeDecl, so findIndexer returns null and both branches are skipped.
// The new branch (c) added by this fix calls lowerCollectionIndexerGet to
// read the current value, computes ±1, then calls lowerCollectionIndexerSet to
// write back — reusing the same encode/decode helpers as the compound-assign
// and plain-set paths so the fix is minimal and consistent.
//
// For compound `arr[i] += n` the #508 fix (lowerCollectionIndexerCompound)
// already handled Array<T>; for Dictionary<K,V> that path guards with
// `isArr` only, so `dict[k] += n` is also a no-op. The same branch (c) fix
// covers dict compound-increment as well.
//
// Spec authority: §25.1 (read-modify-write through indexer), §4.4 (Array<T>
// indexer get/set), §4.8 (Dictionary<K,V> indexer get/set).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// ---------------------------------------------------------------------------
// Array<int> ++ / -- / += — §4.4 / §25.1
// ---------------------------------------------------------------------------

// Primary repro for #613: arr[0]++ on a heap Array<int> must increment the
// element and store it back; before the fix it returned 0 (the original value).
BEZEL_TEST(builtin_collection_incdec_613, array_postfix_increment_stores_back_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(0); "
        "    arr[0]++; "
        "    return arr[0]; "  // expected 1; was 0 before fix
        "  end Main; "
        "end C;"), 1);
}

// §4.4 / §25.1: arr[0]-- must decrement and store back.
BEZEL_TEST(builtin_collection_incdec_613, array_postfix_decrement_stores_back_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); "
        "    arr[0]--; "
        "    return arr[0]; "  // expected 9
        "  end Main; "
        "end C;"), 9);
}

// §4.4 / §25.1: multiple increments at the same slot accumulate.
BEZEL_TEST(builtin_collection_incdec_613, array_increment_accumulates_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(0); "
        "    arr[0]++; "
        "    arr[0]++; "
        "    arr[0]++; "
        "    return arr[0]; "  // expected 3
        "  end Main; "
        "end C;"), 3);
}

// §4.4 / §25.1: increment at a non-zero index updates only that slot.
BEZEL_TEST(builtin_collection_incdec_613, array_increment_nonzero_index_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); "  // slot 0
        "    arr.Add(20); "  // slot 1
        "    arr[1]++; "
        "    return arr[0] + arr[1]; "  // 10 + 21 = 31
        "  end Main; "
        "end C;"), 31);
}

// §4.4 / §25.1: increment inside a loop accumulates correctly.
BEZEL_TEST(builtin_collection_incdec_613, array_increment_in_loop_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(0); "
        "    var n := 0; "
        "    while n < 5 do "
        "      arr[0]++; "
        "      n++; "
        "    end; "
        "    return arr[0]; "  // expected 5
        "  end Main; "
        "end C;"), 5);
}

// §4.4 / §25.1: `arr[i] += n` on a heap Array<int> — compound-increment
// variant. The #508 fix covered `+=` for arrays (lowerCollectionIndexerCompound);
// this test is a regression guard confirming it still works after #613.
BEZEL_TEST(builtin_collection_incdec_613, array_compound_plus_equals_regression_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(10); "
        "    arr[0] += 5; "
        "    return arr[0]; "  // 15
        "  end Main; "
        "end C;"), 15);
}

// ---------------------------------------------------------------------------
// Dictionary<string,int> ++ / += — §4.8 / §25.1
// ---------------------------------------------------------------------------

// §4.8 / §25.1: dict["k"]++ on a Dictionary<string,int> must increment the
// stored integer value and write it back; before the fix it was a no-op.
BEZEL_TEST(builtin_collection_incdec_613, dict_str_int_postfix_increment_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new Dictionary<string, int>(); "
        "    d.Add(\"k\", 0); "
        "    d[\"k\"]++; "
        "    return d[\"k\"]; "  // expected 1; was 0 before fix
        "  end Main; "
        "end C;"), 1);
}

// §4.8 / §25.1: dict["k"]-- must decrement and store back.
BEZEL_TEST(builtin_collection_incdec_613, dict_str_int_postfix_decrement_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new Dictionary<string, int>(); "
        "    d.Add(\"k\", 10); "
        "    d[\"k\"]--; "
        "    return d[\"k\"]; "  // expected 9
        "  end Main; "
        "end C;"), 9);
}

// §4.8 / §25.1: dict["k"] += n — compound-increment on Dictionary<string,int>.
// Before the fix lowerCollectionIndexerCompound only handled arrays; dict was
// skipped and the write-back was silently dropped.
BEZEL_TEST(builtin_collection_incdec_613, dict_str_int_compound_plus_equals_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new Dictionary<string, int>(); "
        "    d.Add(\"k\", 10); "
        "    d[\"k\"] += 5; "
        "    return d[\"k\"]; "  // expected 15; was 10 before fix
        "  end Main; "
        "end C;"), 15);
}

// §4.8 / §25.1: multiple increments accumulate on the same dict key.
BEZEL_TEST(builtin_collection_incdec_613, dict_str_int_increment_accumulates_4_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    using var d := new Dictionary<string, int>(); "
        "    d.Add(\"x\", 0); "
        "    d[\"x\"]++; "
        "    d[\"x\"]++; "
        "    return d[\"x\"]; "  // expected 2
        "  end Main; "
        "end C;"), 2);
}

// ---------------------------------------------------------------------------
// Regression: user-defined indexer ++ still works — §20.5 / #587
// ---------------------------------------------------------------------------

// §20.5: user-defined indexer `v[0]++` must still call the setter.
// Confirms that branch (c) for built-in collections does not disturb branch (b)
// for user-defined indexers (they are tried in order; (b) runs first for
// user-defined, (c) runs for built-in — no overlap).
BEZEL_TEST(builtin_collection_incdec_613, user_indexer_increment_regression_20_5) {
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

// ---------------------------------------------------------------------------
// Regression: plain local int ++ still works
// ---------------------------------------------------------------------------

// Confirms that the new branch (c) is only taken for heap-collection IndexExpr
// targets and does not interfere with plain local increment.
BEZEL_TEST(builtin_collection_incdec_613, local_int_increment_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var x := 3; x++; return x; "
        "  end Main; "
        "end C;"), 4);
}

// ---------------------------------------------------------------------------
// Regression: inline array d[0]++ still works — §15.1
// ---------------------------------------------------------------------------

// Inline arrays have lvalueAddr → GEP → plain store; the new branch (c) must
// not intercept them (isCollectionTypeName is false for [N]T).
BEZEL_TEST(builtin_collection_incdec_613, inline_array_increment_regression_15_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: [2]int; d[0] := 5; d[0]++; return d[0]; "
        "  end Main; "
        "end C;"), 6);
}

// ---------------------------------------------------------------------------
// §17.6 (#1299): the index subexpression of `arr[i]++` / `arr[i]--` is
// evaluated EXACTLY ONCE. lowerCollectionIndexerIncrDecr read via
// lowerCollectionIndexerGet then wrote via lowerCollectionIndexerSet(*ix, …),
// each re-lowering ix->target and ix->indices[0].value — so a side-effecting
// index (`arr[Idx()]++`) fired twice: the get read one slot, the set
// incremented another. Idx() counts its calls via the static N.
// ---------------------------------------------------------------------------

// `arr[Idx()]++`: Idx() must fire once (returns 0) → arr[0] becomes 11,
// arr[1] stays 20, N == 1. 11*1000 + 20*10 + 1 = 11201.
BEZEL_TEST(builtin_collection_incdec_613, array_increment_index_evaluated_once_17_6) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static N: int; "
        "  public static method Idx(): int "
        "    var r := N; N := N + 1; return r; "
        "  end Idx; "
        "  public static method Main(): int "
        "    N := 0; "
        "    var arr := new Array<int>(); "
        "    arr.Add(10); arr.Add(20); "
        "    arr[Idx()]++; "
        "    Console.WriteLine(arr[0] * 1000 + arr[1] * 10 + N); return 0; "  // 11201
        "  end Main; "
        "end C;"), "11201\n");
}

// `arr[Idx()]--`: single evaluation → arr[0] becomes 9, arr[1] stays 20,
// N == 1. 9*1000 + 20*10 + 1 = 9201.
BEZEL_TEST(builtin_collection_incdec_613, array_decrement_index_evaluated_once_17_6) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static N: int; "
        "  public static method Idx(): int "
        "    var r := N; N := N + 1; return r; "
        "  end Idx; "
        "  public static method Main(): int "
        "    N := 0; "
        "    var arr := new Array<int>(); "
        "    arr.Add(10); arr.Add(20); "
        "    arr[Idx()]--; "
        "    Console.WriteLine(arr[0] * 1000 + arr[1] * 10 + N); return 0; "  // 9201
        "  end Main; "
        "end C;"), "9201\n");
}
