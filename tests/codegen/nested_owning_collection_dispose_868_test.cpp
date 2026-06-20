// fix #868 / §28.11: nested owning collections (e.g. OwnedArray<OwnedArray<R>>)
// must dispose their inner collections, which in turn dispose their elements.
//
// Root cause: elementPolicyFor classified builtin owning-collection element types
// (OwnedArray/OwnedStack/OwnedQueue/OwnedLinkedList/OwnedDictionary/OwnedSet, all
// with declaration==nullptr) as plain Value — no disposer → the outer collection
// never freed its inner collections → leak. The fix adds a name-detection path
// before the class/declaration check, mirroring the Shared/IGrouping/Subscription
// handling, and builds a nestedCollectionDisposerThunk that calls the right runtime
// free fn (bz_array_free / bz_llist_free / bz_dict_free) with the recursively
// resolved inner-element disposer.
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// Helper Bezel snippets re-used across tests.
static const char* kRClass =
    "class R public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; end R; ";

// §28.11, #868 — baseline: OwnedArray<OwnedArray<R>> with one inner R → R._n = 1
BEZEL_TEST(nested_owning_collection_dispose_868, basic_nested_array) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kRClass +
        "class C public static method Main(): int "
        "  do "
        "    using var outer := new OwnedArray<OwnedArray<R>>(); "
        "    var inner := new OwnedArray<R>(); "
        "    inner.Add(move (new R())); "
        "    outer.Add(move inner); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 1);
}

// §28.11, #868 — two inner arrays each with one R → R._n = 2 (both freed)
BEZEL_TEST(nested_owning_collection_dispose_868, two_inner_arrays) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kRClass +
        "class C public static method Main(): int "
        "  do "
        "    using var outer := new OwnedArray<OwnedArray<R>>(); "
        "    var inner1 := new OwnedArray<R>(); "
        "    inner1.Add(move (new R())); "
        "    outer.Add(move inner1); "
        "    var inner2 := new OwnedArray<R>(); "
        "    inner2.Add(move (new R())); "
        "    outer.Add(move inner2); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 2);
}

// §28.11, #868 — three levels deep: OwnedArray<OwnedArray<OwnedArray<R>>> → R._n = 1
BEZEL_TEST(nested_owning_collection_dispose_868, deep_three_levels) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kRClass +
        "class C public static method Main(): int "
        "  do "
        "    using var outer := new OwnedArray<OwnedArray<OwnedArray<R>>>(); "
        "    var mid := new OwnedArray<OwnedArray<R>>(); "
        "    var inner := new OwnedArray<R>(); "
        "    inner.Add(move (new R())); "
        "    mid.Add(move inner); "
        "    outer.Add(move mid); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 1);
}

// §28.11, #868 — OwnedStack<OwnedArray<R>> → inner array and its R both disposed
BEZEL_TEST(nested_owning_collection_dispose_868, owned_stack_outer) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kRClass +
        "class C public static method Main(): int "
        "  do "
        "    using var outer := new OwnedStack<OwnedArray<R>>(); "
        "    var inner := new OwnedArray<R>(); "
        "    inner.Add(move (new R())); "
        "    outer.Push(move inner); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 1);
}

// §28.11, #868 — OwnedLinkedList<OwnedArray<R>> → inner array and R disposed
BEZEL_TEST(nested_owning_collection_dispose_868, owned_linkedlist_outer) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kRClass +
        "class C public static method Main(): int "
        "  do "
        "    using var outer := new OwnedLinkedList<OwnedArray<R>>(); "
        "    var inner := new OwnedArray<R>(); "
        "    inner.Add(move (new R())); "
        "    outer.AddLast(move inner); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 1);
}

// §28.11, #868 — no-double-free: 2 elements; R._n must be exactly 2 (not >2).
// A double-free would crash or over-count; crash is rc=-999.
BEZEL_TEST(nested_owning_collection_dispose_868, no_double_free_two_elements) {
    int v = run(std::string("module M; ") + kRClass +
        "class C public static method Main(): int "
        "  do "
        "    using var outer := new OwnedArray<OwnedArray<R>>(); "
        "    var inner1 := new OwnedArray<R>(); "
        "    inner1.Add(move (new R())); "
        "    outer.Add(move inner1); "
        "    var inner2 := new OwnedArray<R>(); "
        "    inner2.Add(move (new R())); "
        "    outer.Add(move inner2); "
        "  end; "
        "  return R._n; "
        "end Main; end C;");
    // Must be exactly 2: 1 per R disposed. >2 = double-free; 0 = leak; crash = -999.
    BEZEL_EXPECT_EQ(v, 2);
}

// §28.11, #868 — borrow case: Array<OwnedArray<R>> (borrowing outer) must NOT
// dispose the inner arrays. The real owner (`using var a`) disposes it once.
// R._n = 1 (disposed by the owner, not the borrower).
BEZEL_TEST(nested_owning_collection_dispose_868, borrow_outer_no_dispose) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kRClass +
        "class C public static method Main(): int "
        "  do "
        "    using var a := new OwnedArray<R>(); "
        "    a.Add(move (new R())); "
        "    do "
        "      using var borrower := new Array<OwnedArray<R>>(); "
        "    end; "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 1);
}

// §28.11, #868 — regression: single-level OwnedArray<R> still works (no regression).
BEZEL_TEST(nested_owning_collection_dispose_868, single_level_control) {
    BEZEL_EXPECT_EQ(run(std::string("module M; ") + kRClass +
        "class C public static method Main(): int "
        "  do "
        "    using var arr := new OwnedArray<R>(); "
        "    arr.Add(move (new R())); "
        "  end; "
        "  return R._n; "
        "end Main; end C;"), 1);
}
