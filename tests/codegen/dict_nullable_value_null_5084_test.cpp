// Regression tests for issue #5084 — Dictionary<K, V?> null-value literal and
// overwrite corrupts/crashes (codegen).
//
// Spec: stdlib §4.8 (Dictionary<K,V>) — "K must be hashable ... V is
// unconstrained", so V may be a nullable VALUE type (int?, long?, bool?,
// enum?, nullable record), which lowers to a tagged struct {i8 hasValue; T}
// (§7.2 nullable value types) and is heap-boxed the same way a non-nullable
// record value is. Language spec §5.1 "Nullable conversions": "for a value
// type, the present-value wrapping" — every site that stores a V? value must
// perform that wrapping, INCLUDING a dictionary-LITERAL entry, not just an
// explicit `.Add`/indexer-set call.
//
// --- Bug A (issue #5084) ---------------------------------------------------
// `materializeHeapDictLiteral`'s own value encode (`encodeVal`,
// LlvmBackend_Setup.cpp) had NO nullable-value promotion at all — unlike
// `lowerCollectionIndexerSet`'s `encodeValue` (LlvmBackend_Assign.cpp), which
// already boxed a null V? payload as `nullableValueNull(valTy)` (issue #544).
// A `null` entry in a dict literal (`["k": null]` against
// `Dictionary<string, long?>`) therefore passed a bare LLVM `ptr null`
// straight through as the stored "value" pointer. MEASURED: compiles cleanly
// (exit 0, no diagnostics) but SEGVs on the very first read of the entry.
//
// --- Bug B (issue #5084) ----------------------------------------------------
// The issue's own report suspected a SEPARATE defect in the disposing
// overwrite runtime call (`bz_dict_set_str_disposing`). MEASURED (this fix's
// own root-cause pass, via `BEZEL_DUMP_IR=1` and an ASan probe): it is the
// SAME root cause as Bug A, just triggered by a NON-null literal entry. The
// dict-literal `encodeVal`'s missing promotion meant even a plain scalar
// literal (`["k": 99]` against `Dictionary<string, long?>`) was stored as a
// raw, untagged `inttoptr(99)` rather than the boxed `{i8=1,i64=99}` every
// other write path produces. A later `d["k"] := null;` overwrite correctly
// boxes the new `null` (via the ALREADY-correct indexer-set `encodeValue`),
// calls `bz_dict_set_str_disposing`, which frees the OLD value under the key
// before storing the new one — and the "old value" is the bogus untagged `99`
// the literal stored, so the runtime `free()`s address `0x63` (99 decimal):
// SIGABRT/SEGV ("free(): invalid pointer" without ASan; ASan reports a
// deallocation SEGV in `bz$valBoxFree` <- `bz_dict_set_str_disposing`).
// Independently confirmed: reading back a literal-inserted non-null value
// with NO overwrite at all (`["k": 99]` then `d["k"]`) also SEGVs, on
// dereferencing the same bogus `0x63` "box" pointer — proving the defect is
// in the literal's ENCODE, not in the disposing setter.
//
// --- Fix --------------------------------------------------------------------
// The nullable-value promotion block that used to live inline in
// `lowerCollectionIndexerSet`'s `encodeValue` lambda (LlvmBackend_Assign.cpp)
// is hoisted to a shared member function, `Emitter::liftDictValueToNullableStruct`
// (declared in LlvmBackend_impl.hpp, defined in LlvmBackend_Assign.cpp right
// above `lowerCollectionIndexerSet`). Both `encodeValue` (indexer-set,
// LlvmBackend_Assign.cpp) and `encodeVal` (dictionary literal,
// `materializeHeapDictLiteral`, LlvmBackend_Setup.cpp) now call this ONE
// function before their own struct-boxing decision
// (`dictValueIsBoxedStruct`), so a dictionary literal and an indexer-set box
// a V? payload — null or present — identically.
//
// Tests are load-bearing: before the fix, every "literal ... null" or
// "literal ... nonnull ... read/overwrite" row below either SEGVs or aborts;
// after the fix they all print the expected line and exit 0.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// ---------------------------------------------------------------------------
// Bug A — the issue's exact repro (string-keyed Dictionary<string, long?>,
// literal `null` value, no overwrite). Pre-fix: SEGV on the first read.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_nullable_value_null_5084, bugA_issue_repro_literal_null_value_read_spec_4_8_5_1) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var d: Dictionary<string, long?> := [\"k\": null]; "
        "    if d[\"k\"] = null then "
        "      Console.WriteLine(\"lit null ok\"); "
        "    else "
        "      Console.WriteLine(\"lit not-null\"); "
        "    end if; "
        "  end Main; "
        "end C;",
        "lit null ok\n");
}

// ---------------------------------------------------------------------------
// Bug B — the issue's exact repro (overwrite an existing literal-built
// non-null value with `null`). Pre-fix: SIGABRT ("free(): invalid pointer").
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_nullable_value_null_5084, bugB_issue_repro_literal_then_overwrite_null_spec_4_8_5_1) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var d: Dictionary<string, long?> := [\"k\": 99]; "
        "    d[\"k\"] := null; "
        "    if d[\"k\"] = null then "
        "      Console.WriteLine(\"ow null ok\"); "
        "    else "
        "      Console.WriteLine(\"ow not-null\"); "
        "    end if; "
        "  end Main; "
        "end C;",
        "ow null ok\n");
}

// ---------------------------------------------------------------------------
// The SHARED root cause, isolated: a literal non-null value, read back with
// NO overwrite at all. Pre-fix: SEGV dereferencing the bogus untagged `99`.
// Proves the defect is in the literal's own encode, independent of the
// disposing-overwrite runtime call the issue originally suspected.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_nullable_value_null_5084, literal_nonnull_value_read_back_no_overwrite_spec_4_8_5_1) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var d: Dictionary<string, long?> := [\"k\": 99]; "
        "    var v: long? := d[\"k\"]; "
        "    if v = null then "
        "      Console.WriteLine(\"read null\"); "
        "    else "
        "      Console.WriteLine($\"read {v!.ToString()}\"); "
        "    end if; "
        "  end Main; "
        "end C;",
        "read 99\n");
}

// ---------------------------------------------------------------------------
// Generic (int) key path — the dict-literal generic-key branch
// (bz_dict_set_gen) shares the SAME encodeVal lambda, so it must be fixed
// identically. Pre-fix: same SEGV shape as the string-key path.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_nullable_value_null_5084, int_keyed_literal_null_value_read_spec_4_8_5_1) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var d: Dictionary<int, long?> := [1: null]; "
        "    if d[1] = null then "
        "      Console.WriteLine(\"intkey lit null ok\"); "
        "    else "
        "      Console.WriteLine(\"intkey lit not-null\"); "
        "    end if; "
        "  end Main; "
        "end C;",
        "intkey lit null ok\n");
}

BEZEL_TEST(dict_nullable_value_null_5084, int_keyed_literal_then_overwrite_null_spec_4_8_5_1) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var d: Dictionary<int, long?> := [1: 7]; "
        "    d[1] := null; "
        "    if d[1] = null then "
        "      Console.WriteLine(\"intkey ow null ok\"); "
        "    else "
        "      Console.WriteLine(\"intkey ow not-null\"); "
        "    end if; "
        "  end Main; "
        "end C;",
        "intkey ow null ok\n");
}

// ---------------------------------------------------------------------------
// Nullable RECORD value (Item?) — a "wider" boxed nullable value type than a
// bare primitive (§9 records / §7.2). Same literal encode path, same
// dictValueIsBoxedStruct boxing decision downstream.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_nullable_value_null_5084, nullable_record_literal_null_value_read_spec_4_8_7_9) {
    expectPrints(
        "module M; record Item(Score: int); "
        "public class C "
        "  public static method Main() "
        "    var d: Dictionary<string, Item?> := [\"k\": null]; "
        "    if d[\"k\"] = null then "
        "      Console.WriteLine(\"rec lit null ok\"); "
        "    else "
        "      Console.WriteLine(\"rec lit not-null\"); "
        "    end if; "
        "  end Main; "
        "end C;",
        "rec lit null ok\n");
}

BEZEL_TEST(dict_nullable_value_null_5084, nullable_record_literal_nonnull_then_overwrite_null_spec_4_8_7_9) {
    expectPrints(
        "module M; record Item(Score: int); "
        "public class C "
        "  public static method Main() "
        "    var d: Dictionary<string, Item?> := [\"k\": new Item(42)]; "
        "    var before: Item? := d[\"k\"]; "
        "    d[\"k\"] := null; "
        "    var after: Item? := d[\"k\"]; "
        "    var beforeScore := (if before = null then -1 else before!.Score); "
        "    var afterIsNull := (if after = null then 1 else 0); "
        "    Console.WriteLine($\"rec before {beforeScore} afterNull {afterIsNull}\"); "
        "  end Main; "
        "end C;",
        "rec before 42 afterNull 1\n");
}

// ---------------------------------------------------------------------------
// CONTROL — a nullable CLASS-typed value (`class?`) is NOT a nullable VALUE
// type (isNullableValueTy is false for it: it lowers to a plain nullable
// pointer, not a tagged struct), so `liftDictValueToNullableStruct` must be a
// no-op for it. This was never part of #5084 (the issue's own triage notes
// "Array<T?> literals ... are unaffected"); pinned here as a regression guard
// so the shared lift cannot start mis-handling the pointer-null case.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_nullable_value_null_5084, control_nullable_class_value_literal_null_unaffected_spec_4_8) {
    expectPrints(
        "module M; public class Widget public _tag: int := 0; end Widget; "
        "public class C "
        "  public static method Main() "
        "    var d: Dictionary<string, Widget?> := [\"k\": null]; "
        "    if d[\"k\"] = null then "
        "      Console.WriteLine(\"class lit null ok\"); "
        "    else "
        "      Console.WriteLine(\"class lit not-null\"); "
        "    end if; "
        "  end Main; "
        "end C;",
        "class lit null ok\n");
}

// ---------------------------------------------------------------------------
// Loop-amplified (200 iterations): builds a FRESH literal dict each
// iteration, overwrites null->non-null->null, and re-reads. A one-shot repro
// can pass by accident (LSan reports only unreachable allocations, and a
// single free-of-garbage can coincidentally land in mapped memory); looping
// stresses both the box malloc/free cycle and cumulative memory (LSan-clean
// under the ASan regression run) the way a single iteration cannot.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_nullable_value_null_5084, loop_amplified_literal_and_overwrite_200_iterations_spec_4_8_5_1) {
    expectPrints(
        "module M; "
        "public class C "
        "  public static method Main() "
        "    var n: int := 0; "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      var d: Dictionary<string, long?> := [\"k\": null]; "
        "      if d[\"k\"] = null then n := n + 1; end if; "
        "      d[\"k\"] := 42; "
        "      if d[\"k\"] <> null then n := n + 1; end if; "
        "      d[\"k\"] := null; "
        "      if d[\"k\"] = null then n := n + 1; end if; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"loop {n}\"); "
        "  end Main; "
        "end C;",
        "loop 600\n");
}
