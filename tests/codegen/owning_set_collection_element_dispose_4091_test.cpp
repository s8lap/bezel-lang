// fix #4091 / §28.11 / §28.9 / stdlib §4.10 / §4.4 / §4.5: an OwningSet<T>'s
// element disposal cascade must reclaim a stored element whose TYPE is itself
// a BORROWING collection (Array<T>, Stack<T>, Queue<T>, LinkedList<T>,
// FrozenArray<T>, Dictionary<K,V>, SortedDictionary<K,V>) — the OwningSet
// twin of #4054 (which covered OwningArray). OwningSet<T> admits a class-typed
// T, and class-typed T is REJECTED by genericBackingType (its `isSet` guard,
// see LlvmBackend_Calls3.cpp:407), so OwningSet<T> for a user-class T stays
// array-backed (the existing #4054 cascade applies via emitArrayBackedSetInsert).
//
// The OwningSet<T> / borrowing-builtin-collection case is the residual: every
// borrowing built-in collection is a compiler-known builtin
// (declaration == nullptr, single-part name like "Array"/"Stack"/...); the
// genericBackingType predicate's "unresolved type parameter" fallback
// (LlvmBackend_Calls3.cpp:347-355) was originally written to give the
// unspecialized generic body a generic-key table — but it ALSO admits the
// builtin collections (same shape: Named, no declaration, single-part name).
// So OwningSet<Array<int>> allocates the table via bz_dict_alloc_generic
// (the `setDictBacked` path), each Add inserts an Array<int>* pointer as a
// 8-byte generic-key key with a sentinel value, and the scope-end
// bz_dict_free(tbl, null) frees only the key copies — every stored
// Array<int> leaks its bz_array_t header + data buffer. Measured on
// origin/main 2d201b3 (the post-#4054 state): 801 × (56 + 64) = 96,120 bytes
// leaked for an 800-iteration OwningSet<Array<int>> loop. OwningSet is the
// twin / sister issue.
//
// Fix: mirror #4054's pattern in the OwningSet disposal cascade. The
// setDictBacked arm in registerUsingDisposal now consults elementPolicyFor
// with owningFamily=true on the element TypeRef; when the element is a
// borrowing builtin collection (the #4054 borrowing-builtin arm), the policy
// returns a ClassOwned category with the borrowingCollectionDisposerThunk
// (per #4054). That thunk is stashed in the new Disposal::dictKeyDisposer
// field. At scope end the disposal emits
//   bz_set_coll_keys_dispose(tbl, dictKeyDisposer);  // fix #4091, NEW helper
//   bz_dict_free(tbl, null);
// — the helper iterates the table's live keys, reads each as a void* pointer
// to an inner collection header, and calls the disposer (which invokes
// bz_array_free / bz_llist_free / bz_dict_free with NULL inner disposer per
// #4054). The subsequent bz_dict_free reclaims the key copies and the table
// itself. The Set<T> borrowing shape is unaffected (Set borrows, never sets
// dictKeyDisposer); SortedSet<T> is rejected by sema (where T: IComparable<T>
// excludes built-in collections).
//
// FAIL-WITHOUT (issue #4091's own repro, verbatim): an 800-iteration
// OwningSet<Array<int>> loop leaks 96,120 B (56 B × 801 + 64 B × 801) of
// bz_array_t headers + data buffers. LSan reports the leak; `runAotChecked`
// returns ran=false. PASS-WITH: the loop runs clean and `total mod 251` = 48
// (801 × 1 = 801; 801 mod 251 = 48 — matches the OwningArray<Array<int>> twin
// from #4054, byte-for-byte).

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
BEZEL_TEST(owning_set_collection_element_dispose_4091, array_int_verbatim) {
    std::string src = std::string(kPreamble) +
        "    var total := 0; "
        "    for var i in 0..800 do "
        "      using var bucket := new OwningSet<Array<int>>(); "
        "      var items := new Array<int>(); "
        "      items.Add(1); items.Add(2); items.Add(3); "
        "      bucket.Add(move items); "
        "      total := total + bucket.Count; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 48);
}

// 2. Stack<int> — same bz_array_t backing, different head name. Verifies the
//    name-detection branch covers the whole borrowing built-in family for the
//    OwningSet (so the cascade reaches each distinct head's free fn).
//    `0..200` → 201 iterations; 201 mod 251 = 201.
BEZEL_TEST(owning_set_collection_element_dispose_4091, stack_int) {
    std::string src = std::string(kPreamble) +
        "    var total := 0; "
        "    for var i in 0..200 do "
        "      using var bucket := new OwningSet<Stack<int>>(); "
        "      var items := new Stack<int>(); "
        "      items.Push(1); items.Push(2); "
        "      bucket.Add(move items); "
        "      total := total + bucket.Count; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 201);
}

// 3. Queue<int>.
BEZEL_TEST(owning_set_collection_element_dispose_4091, queue_int) {
    std::string src = std::string(kPreamble) +
        "    var total := 0; "
        "    for var i in 0..200 do "
        "      using var bucket := new OwningSet<Queue<int>>(); "
        "      var items := new Queue<int>(); "
        "      items.Enqueue(1); items.Enqueue(2); items.Enqueue(3); "
        "      bucket.Add(move items); "
        "      total := total + bucket.Count; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 201);
}

// 4. LinkedList<int> — DIFFERENT runtime free fn (bz_llist_free). Verifies
//    the borrowingCollectionDisposerThunk's name-based dispatch reaches
//    bz_llist_free for "LinkedList" when called from the OwningSet cascade.
BEZEL_TEST(owning_set_collection_element_dispose_4091, linkedlist_int) {
    std::string src = std::string(kPreamble) +
        "    var total := 0; "
        "    for var i in 0..200 do "
        "      using var bucket := new OwningSet<LinkedList<int>>(); "
        "      var items := new LinkedList<int>(); "
        "      items.AddLast(1); items.AddLast(2); "
        "      bucket.Add(move items); "
        "      total := total + bucket.Count; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 201);
}

// 5. Dictionary<int, int> — DIFFERENT runtime free fn (bz_dict_free). Verifies
//    the borrowingCollectionDisposerThunk's name-based dispatch reaches
//    bz_dict_free for "Dictionary" / "SortedDictionary".
BEZEL_TEST(owning_set_collection_element_dispose_4091, dictionary_int_int) {
    std::string src = std::string(kPreamble) +
        "    var total := 0; "
        "    for var i in 0..200 do "
        "      using var bucket := new OwningSet<Dictionary<int, int>>(); "
        "      var items := new Dictionary<int, int>(); "
        "      items.Add(1, 1); items.Add(2, 2); "
        "      bucket.Add(move items); "
        "      total := total + bucket.Count; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 201);
}

// 6. SortedDictionary<int, int> — DIFFERENT runtime free fn (bz_dict_free).
BEZEL_TEST(owning_set_collection_element_dispose_4091, sorted_dictionary_int_int) {
    std::string src = std::string(kPreamble) +
        "    var total := 0; "
        "    for var i in 0..200 do "
        "      using var bucket := new OwningSet<SortedDictionary<int, int>>(); "
        "      var items := new SortedDictionary<int, int>(); "
        "      items.Add(1, 1); items.Add(2, 2); "
        "      bucket.Add(move items); "
        "      total := total + bucket.Count; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 201);
}

// 7. NO-DOUBLE-FREE GUARD — Set<R> (borrowing) outer with R a class with a
//    destructor counter; OwningSet<R2> inner (the actual case from #4054,
//    adapted to OwningSet's borrowing shape). The OwningSet<Array<R2>>'s
//    cascade must free the Array<R2>'s backing (the leak we're fixing) but
//    MUST NOT dispose the R2 elements (Array borrows them — disposing them
//    here would double-free). R2._n counter proves WHICH R2's were disposed:
//    each iteration creates two R2's as `using var` locals; their scope end
//    at the bottom of the loop body, disposing both exactly once. The
//    OwningSet<Array<R2>> holds borrows (via Array<R2>) of those R2's, and
//    its own disposal must NOT increment R2._n (would mean double-dispose).
//    After 201 iterations, R2._n == 201 × 2 = 402 (exit code = 402 mod 256
//    = 146). The LSan oracle backs this up — ASan detects any double-free
//    as a sanitizer report.
BEZEL_TEST(owning_set_collection_element_dispose_4091, no_double_free_borrowed_elems) {
    constexpr const char* kSrc =
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C "
        "  public static method Main(): int "
        "    for var i in 0..200 do "
        "      using var bucket := new OwningSet<Array<R>>(); "
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

// 8. NEGATIVE — a borrowing Set<Array<int>> local: the OUTER is borrowing,
//    so the cascade does NOT dispose anything. Verifies the fix's gating
//    `if (collIsOwning && ...)` correctly does NOT install dictKeyDisposer
//    for the borrowing shape — installing it would be a double-free (the
//    inner Array<int> is owned elsewhere — here, by `items` as a `using
//    var`, which disposes it at the bottom of the loop body). The test is
//    pure soundness: nothing should be disposed by the Set's scope end, no
//    crash, no over-dispose.
BEZEL_TEST(owning_set_collection_element_dispose_4091, borrow_outer_borrow_inner) {
    std::string src = std::string(kPreamble) +
        "    for var i in 0..50 do "
        "      using var bucket := new Set<Array<int>>(); "
        "      using var items := new Array<int>(); "
        "      items.Add(1); "
        "      bucket.Add(items); "
        "    end for; "
        "    return 7; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 7);
}

// 9. LOOP-AMPLIFIED OWNING-SET ARRAY-INT — 2001 iterations × (Array<int>
//    inner). The 801-byte measurement (96,120 B) on the issue's verbatim
//    800-iteration repro proves the bug; the 2001-iteration amplification
//    raises it to ~240,420 B, comfortably above any single-iteration noise
//    floor. LSan-clean post-fix. The `0..2000` form runs 2001 iterations;
//    total = 2001; 2001 mod 251 = 2001 - 7*251 = 2001 - 1757 = 244.
BEZEL_TEST(owning_set_collection_element_dispose_4091, loop_amplified_2000x_array_int) {
    std::string src = std::string(kPreamble) +
        "    var total := 0; "
        "    for var i in 0..2000 do "
        "      using var bucket := new OwningSet<Array<int>>(); "
        "      var items := new Array<int>(); "
        "      items.Add(1); items.Add(2); items.Add(3); "
        "      bucket.Add(move items); "
        "      total := total + bucket.Count; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 244);
}

}  // namespace