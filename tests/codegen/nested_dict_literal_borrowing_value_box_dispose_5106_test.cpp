// Codegen / disposal — issue #5106 (§28.9 / §28.11, v0.307):
// A nested `Dictionary<K, V>` literal where V is itself a BORROWING builtin
// collection (Dictionary / SortedDictionary / Array / Stack / Queue /
// LinkedList / FrozenArray) leaks the inner table when the outer is released.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Disposal.cpp, dictValueBoxFreer).
// When V is a borrowing builtin collection the outer Dictionary<K, V>'s
// value slot stores the inner collection's pointer (the inner's `bz_dict_t*`
// / `bz_array_t*` etc.) DIRECTLY — no boxing. The boxed-struct early-return
// in dictValueBoxFreer (`!dictValueIsBoxedStruct(valLlvm) → nullptr`) fires
// for these V because the borrowed inner lowers to `i8*`, not a struct, so
// without this fix the function returned nullptr, `buildCollectionTempDisposal`
// (LlvmBackend_Setup.cpp) left `dictValueDisposer = nullptr`, and the
// outer's `bz_dict_free(outer, nullptr)` skipped every stored inner
// bz_dict_t / bz_array_t / bz_llist_t — measured 422 B / 4 allocations per
// repro (96 B inner header + 256 B inner buckets + 64 B inner order + 6 B
// inner "inner" key), ASan/LSan direct leak at scope end.
//
// FIX. dictValueBoxFreer now recognises a non-nullable V that is itself a
// borrowing builtin collection BEFORE the boxed-struct early-return and
// returns `borrowingCollectionDisposerThunk(valTy, name)` — the same thunk
// the §28.11 element-policy table emits for an OwningArray<Array<T>> slot
// (#4054). The thunk calls the inner's matching runtime free fn
// (`bz_dict_free` for Dictionary/SortedDictionary, `bz_array_free` for
// Array/Stack/Queue/FrozenArray, `bz_llist_free` for LinkedList) with NULL
// inner disposer — the borrow family does not own its elements; passing
// the inner's element disposer would double-free values whose real owner
// is elsewhere.
//
// Fails-without / passes-with: before the fix each program below LEAKS the
// inner table at scope end, which runAotChecked's LeakSanitizer net turns
// into r.ran==false → run()==-999 (never a legal value). After the fix
// every allocation is freed exactly once (LSan-clean) and the test value
// is returned. The test deliberately aliases the inner through
// `var inner := d["outer"]` so the literal-tree aggregate is disabled —
// without that alias the literal-tree walker processes the inner and
// frees it via a different path (#2107) that masks #5106.
//
// Narrow: nullable V / OwningDictionary<K, …> — sema rejects those shapes
// at instantiation (§28.11) and would need a separate wrapper-walk fix
// (#3802's nullable-record shape), out of scope here. The non-nullable
// borrowing-Dictionary V shape is the one the literal-tree alias
// (var inner := d["outer"]) actually exercises.
//
// Spec: §28.9 (owning-side: a borrowing collection's value slot stores a
// pointer to the inner collection), §28.11 (element-slot table: the dispose
// of a borrowing-builtin-collection V is the matching runtime free fn with
// NULL inner disposer), §2.8.3 (rule 3: mangle by V's identity for
// reproducible thunk names, #3548). Spec revision v0.307.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A leak / double-free / use-after-free flips r.ran to false → -999,
// distinct from any legal exit value.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── Repro 1: nested Dictionary literal where V is Dictionary<string, long>.
// §28.9 / §28.11 (#5106). The inner's bz_dict_t header + buckets + order +
// "inner" key all leak at outer dispose without the fix (422 B / 4 allocs).
// With the fix: every allocation freed exactly once, LSan-clean, returns 99.
BEZEL_TEST(nested_dict_literal_borrowing_value_box_dispose_5106,
           nested_dict_dict_value_disposes_inner_spec_28_9) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var d: Dictionary<string, Dictionary<string, long>> := "
        "      [\"outer\": [\"inner\": 99]]; "
        "    var inner: Dictionary<string, long> := d[\"outer\"]; "
        "    var v: long? := null; "
        "    inner.TryGetValue(\"inner\", out v); "
        "    if v = 99 then return 1; else return 0; end if; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ── Repro 2: nested Dictionary literal where V is Array<long>. Same fix
// path: inner is a borrowing Array<long>, stored by pointer in the outer's
// value slot, freed via bz_array_free(inner, nullptr) at outer dispose.
// Without the fix: bz_array_t header + data buffer leak.
// §28.9 / §28.11 (#5106).
BEZEL_TEST(nested_dict_literal_borrowing_value_box_dispose_5106,
           nested_dict_array_value_disposes_inner_spec_28_9) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var d: Dictionary<string, Array<long>> := "
        "      [\"k\": [10, 20, 30]]; "
        "    var inner: Array<long> := d[\"k\"]; "
        "    var total: long := 0; "
        "    for var x in inner do total := total + x; end; "
        "    if total = 60 then return 1; else return 0; end if; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 1);
}

// ── Repro 3: nested Dictionary literal where V is LinkedList<long>. Inner
// is a bz_llist_t; freed via bz_llist_free(inner, nullptr) at outer dispose.
// Without the fix: bz_llist_t header + nodes leak.
// §28.9 / §28.11 (#5106).
BEZEL_TEST(nested_dict_literal_borrowing_value_box_dispose_5106,
           nested_dict_linkedlist_value_disposes_inner_spec_28_9) {
    std::string src =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var d: Dictionary<string, LinkedList<long>> := "
        "      [\"k\": new LinkedList<long>()]; "
        "    var inner: LinkedList<long> := d[\"k\"]; "
        "    inner.AddLast(7); "
        "    inner.AddLast(8); "
        "    if inner.Count = 2 then return 1; else return 0; end if; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 1);
}
