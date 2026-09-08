// fix #4054 / §28.11 / §28.9 / stdlib §4.4 / §4.5: an OwningArray<T>'s element
// disposal cascade must reclaim a stored element whose TYPE is itself a
// BORROWING collection (Array<T>, Stack<T>, Queue<T>, LinkedList<T>,
// FrozenArray<T>, Dictionary<K,V>, SortedDictionary<K,V>) — not just a class
// (the user-class case has always worked because the declaration-based class
// arm in elementPolicyFor returns ClassOwned + elementDisposerThunk for them).
//
// Root cause: elementPolicyFor's `isOwningBuiltinCollection` predicate (added
// by fix #868) recognised only the OWNING family of built-in collections
// (OwningArray / OwningStack / OwningQueue / OwningLinkedList /
// OwningDictionary / OwningSet). The BORROWING family builtins (Array / Stack
// / Queue / LinkedList / FrozenArray / Dictionary / SortedDictionary / Set /
// SortedSet) all share `declaration == nullptr` (compiler-known builtins, same
// as the Owning family), so they fell past the declaration-based user-class
// arm too — and landed in the Value row at the bottom of elementPolicyFor
// with a null disposerThunk. The OwningArray<T>'s disposal cascade therefore
// called `bz_array_free(arr, NULL)`, freeing only the outer OwningArray's
// header + backing (its element disposer slot is null) but NOT the inner
// borrowing collection's header + backing — LEAK of bz_array_t (56 B) +
// data buffer (8 B per slot, growing on demand) per stored element. Measured
// on origin/main 759b259: 801 × (56 + 64) = 96,120 bytes leaked for an 800-
// iteration OwningArray<Array<int>> loop (the issue's verbatim repro).
//
// Fix: extend elementPolicyFor with a borrowing-builtin-collection arm that
// routes to a per-TypeRef thunk calling the matching runtime free fn
// (bz_array_free / bz_llist_free / bz_dict_free) with NULL inner disposer —
// the borrow family does not own its elements, so the inner disposer is null
// (mirroring #868's recursive inner-element resolution, but with the owning/
// borrowing axis fixed at the OUTER collection: the BORROWING outer borrows,
// so it must not cascade into the inner's elements).
//
// FAIL-WITHOUT (issue #4054's own repro, verbatim): an 800-iteration
// OwningArray<Array<int>> loop leaks 96,120 B (56 B × 801 + 64 B × 801) of
// bz_array_t headers + data buffers. LSan reports the leak; `runAotChecked`
// returns ran=false. PASS-WITH: the loop runs clean and `total mod 251` =
// 47 (800 × 1 = 800; 800 mod 251 = 47).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// runAotChecked — AOT + ASan/LSan oracle. The `r.ran ? r.exitCode : -999`
// idiom returns the program's exit code on success and -999 on a leak (LSan
// report flips ran to false), so a leak fails both the count assertion AND
// the LSan check on the same run.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics()) {
            std::cout << "  diag: " << d.message << "\n";
        }
        return -999;
    }
    return r.exitCode;
}

// Module preamble.
constexpr const char* kPreamble =
    "module M; "
    "class C "
    "  public static method Main(): int ";

// 1. ISSUE REPRO VERBATIM — 800 iterations × (Array<int> inner).
//    `for i in 0..800` runs 801 iterations; total = 801 × 1 = 801;
//    801 mod 251 = 48 (the `0..N` form is inclusive on both ends —
//    not `for i in 0..<N` which would be N iterations).
BEZEL_TEST(owning_array_collection_element_dispose_4054, array_int_verbatim) {
    std::string src = std::string(kPreamble) +
        "    var total := 0; "
        "    for var i in 0..800 do "
        "      using var bucket := new OwningArray<Array<int>>(); "
        "      var items := new Array<int>(); "
        "      items.Add(1); items.Add(2); items.Add(3); "
        "      bucket.Add(move items); "
        "      total := total + bucket.Length; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 48);
}

// 2. Stack<int> — same bz_array_t backing, different head name. Verifies the
//    name-detection branch covers the whole borrowing built-in family.
//    `0..200` → 201 iterations; 201 mod 251 = 201.
BEZEL_TEST(owning_array_collection_element_dispose_4054, stack_int) {
    std::string src = std::string(kPreamble) +
        "    var total := 0; "
        "    for var i in 0..200 do "
        "      using var bucket := new OwningArray<Stack<int>>(); "
        "      var items := new Stack<int>(); "
        "      items.Push(1); items.Push(2); "
        "      bucket.Add(move items); "
        "      total := total + bucket.Length; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 201);
}

// 3. Queue<int>.
BEZEL_TEST(owning_array_collection_element_dispose_4054, queue_int) {
    std::string src = std::string(kPreamble) +
        "    var total := 0; "
        "    for var i in 0..200 do "
        "      using var bucket := new OwningArray<Queue<int>>(); "
        "      var items := new Queue<int>(); "
        "      items.Enqueue(1); items.Enqueue(2); items.Enqueue(3); "
        "      bucket.Add(move items); "
        "      total := total + bucket.Length; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 201);
}

// 4. LinkedList<int> — DIFFERENT runtime free fn (bz_llist_free).
BEZEL_TEST(owning_array_collection_element_dispose_4054, linkedlist_int) {
    std::string src = std::string(kPreamble) +
        "    var total := 0; "
        "    for var i in 0..200 do "
        "      using var bucket := new OwningArray<LinkedList<int>>(); "
        "      var items := new LinkedList<int>(); "
        "      items.AddLast(1); items.AddLast(2); "
        "      bucket.Add(move items); "
        "      total := total + bucket.Length; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 201);
}

// 5. Dictionary<int, int> — DIFFERENT runtime free fn (bz_dict_free).
//    Value type for both K and V so dispose-element side-effects are null and
//    the test is pure leak-oracle.
BEZEL_TEST(owning_array_collection_element_dispose_4054, dictionary_int_int) {
    std::string src = std::string(kPreamble) +
        "    var total := 0; "
        "    for var i in 0..200 do "
        "      using var bucket := new OwningArray<Dictionary<int, int>>(); "
        "      var items := new Dictionary<int, int>(); "
        "      items.Add(1, 1); items.Add(2, 2); "
        "      bucket.Add(move items); "
        "      total := total + bucket.Length; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 201);
}

// 6. NO-DOUBLE-FREE GUARD — Array<R> where R is a class with a destructor
//    counter. The OwningArray's disposal cascade must free the Array<R>'s
//    backing (the leak we're fixing) but MUST NOT dispose the R elements
//    (Array borrows them — disposing them here would double-free).
//
//    R._n counter proves WHICH R's were disposed: each iteration creates two
//    R's as `using var` locals; their scope end at the bottom of the loop
//    body, disposing both exactly once. The OwningArray holds borrows (via
//    Array<R>) of those R's, and its own disposal must NOT increment R._n
//    (would mean double-dispose). After 201 iterations, R._n == 201 × 2 =
//    402 (exit code = 402 mod 256 = 146). The LSan oracle backs this up —
//    ASan detects any double-free as a sanitizer report.
//
//    Note the `using var r := new R()` binding — `items.Add(new R())` would
//    be a §28.3 fresh unbound owned temporary stored into a borrowing-family
//    collection (§15.4 fresh-store warning), the slot holds a borrow of a
//    value that dies at the statement's end. The named binding fixes it.
BEZEL_TEST(owning_array_collection_element_dispose_4054, no_double_free_borrowed_elems) {
    constexpr const char* kSrc =
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C "
        "  public static method Main(): int "
        "    for var i in 0..200 do "
        "      using var bucket := new OwningArray<Array<R>>(); "
        "      var items := new Array<R>(); "
        "      using var r1 := new R(); items.Add(r1); "
        "      using var r2 := new R(); items.Add(r2); "
        "      bucket.Add(move items); "
        "    end for; "
        "    return R._n; "
        "  end Main; "
        "end C;";
    // 201 iterations × 2 Rs each = 402 disposals; exit code is 402 mod 256 = 146.
    BEZEL_EXPECT_EQ(run(kSrc), 146);
}

// 7. NEGATIVE — a borrowing Array<Array<int>> local: the OUTER is borrowing,
//    so the cascade does NOT dispose anything. Verifies elementPolicyFor's
//    new branch returns ClassBorrow (null disposerThunk) when the OUTER is
//    borrowing — the cascade must stop, not over-dispose. The test is pure
//    soundness: nothing should be disposed, no crash, no over-dispose.
BEZEL_TEST(owning_array_collection_element_dispose_4054, borrow_outer_borrow_inner) {
    std::string src = std::string(kPreamble) +
        "    do "
        "      var outer := new Array<Array<int>>(); "
        "      var items := new Array<int>(); "
        "      items.Add(1); "
        "      outer.Add(items); "
        "    end; "
        "    return 7; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 7);
}

}  // namespace