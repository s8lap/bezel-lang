// Codegen — issue #2299 (spec §28.9 / §28.3, v0.236): a method whose declared
// return type is a pure `IEnumerable<T>` and whose body builds and returns a
// LOCAL collection (Array, Set, etc.) silently leaked that collection. The
// RETURN coercion (wrapAsIEnumerableForTarget with ownLazyPipeline=true) wraps
// the source as a vtable-backed enumerator:
//   * lazy LINQ pipeline → OWNING wrapper, owns the drained snapshot (#1916).
//   * dict-backed `Set<T>` → OWNING wrapper over a snapshot, source set left
//     untouched (#1069).
//   * Array/Stack/Queue/LinkedList → BORROWING wrapper, slot-1 Dispose frees
//     only the wrapper frame.
// In every case the source LOCAL's scope-end reclaim was suppressed by §28.3
// ownership-transfer on `return`, so:
//   * the OWNING-wrap case was leak-free for the wrapped data BUT the Array
//     source itself (or the Set source itself) was never freed.
//   * the BORROWING-wrap case leaked the wrapper frame AND the source
//     collection.
// The fix (src/codegen/LlvmBackend_Assign.cpp wrapAsIEnumerableForTarget +
// returnSourceIsCalleeOwnedCollection + freeXxxSourceNow helpers, plumbed
// through the four RETURN-coercion call sites): when the source is callee-
// owned, an Array-family source is wrapped in an OWNING enumerator (its slot-1
// Dispose frees the backing AND the frame exactly once), a dict-backed Set
// source has the source set freed immediately after the snapshot (the
// snapshot is independent), and a Stack/LinkedList source has the source
// freed immediately after the wrap is built (the wrap's slot-1 Dispose runs
// on the caller's side and frees only the wrapper frame).
//
// Verified on main (HEAD `18efbc8`) with `scripts/agent_check.sh --probe
// --asan`:
//
//   module M; class C
//     public static method Seq(): IEnumerable<int>
//       var s := new Array<int>();
//       s.Add(5); s.Add(6);
//       return s;
//     end Seq;
//     public static method Main(): int
//       var n: int := 0;
//       for var x in C.Seq() do n := n + x; end;
//       return n;
//     end Main;
//   end C;
//
//   Direct leak of 40 byte(s) in 1 object(s) (bz_array_alloc)
//   Indirect leak of 64 byte(s) in 1 object(s) (bz_array_alloc)
//   SUMMARY: AddressSanitizer: 104 byte(s) leaked in 2 allocation(s).
//
// The same shape with `new Set<int>()` instead of `new Array<int>()` reports
// 416 byte(s) leaked in 5 allocation(s) (bz_dict_alloc_generic +
// dict_order_append + the key copies), all traced to `bz$M.C$Seq`. After the
// fix both shapes pass runAotChecked with NO leak reported and the asserted
// sums (11 for Array, 11 for Set<int> as 5 + 6).
//
// Fail-without / pass-with: each test below reports a leak (Array: 104B,
// Set: 416B) under LSan on the pre-fix compiler (the -999 sentinel from
// runAotChecked's ASan net); after the fix each returns the asserted VALUE
// with NO leak (exit 0).
// // per spec §28.9, §28.3 (issue #2299)

#include <string>

#include "support/codegen_support.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// runAotChecked runs the linked executable under generated-code ASan; the pre-
// fix leak flips r.ran to false -> -999, distinct from any legal exit value.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// spec §28.9 / fix #2299 — Array<T> RETURNED as IEnumerable<T>
// ===========================================================================

// Repro A — a local Array<int> returned as IEnumerable<int>. The wrap now
// uses an OWNING enumerator (wrapOwnedArrayAsIEnumerable — slot-1 Dispose
// frees the drained array AND the frame); the caller disposes the
// enumerator once. Before the fix: 104B leak (bz_array_alloc x 2, both
// traced to C.Seq). After the fix: leak-free. 5 + 6 = 11.
BEZEL_TEST(return_ienumerable_collection_leak_2299,
           local_array_returned_as_ienumerable_no_leak_spec_28_9) {
    std::string src =
        "module M; class C "
        "  public static method Seq(): IEnumerable<int> "
        "    var s := new Array<int>(); "
        "    s.Add(5); s.Add(6); "
        "    return s; "
        "  end Seq; "
        "  public static method Main(): int "
        "    var n: int := 0; "
        "    for var x in C.Seq() do n := n + x; end; "
        "    return n; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 11);
}

// Inline `return new Array<int>()` — a NewExpr producer (§28.3 K1). The
// OWNING wrap takes ownership of the just-allocated Array; the caller
// disposes the enumerator exactly once. Before the fix: 104B leak. After
// the fix: leak-free.
BEZEL_TEST(return_ienumerable_collection_leak_2299,
           new_array_returned_as_ienumerable_no_leak_spec_28_9) {
    std::string src =
        "module M; class C "
        "  public static method Seq(): IEnumerable<int> "
        "    return new Array<int>(); "
        "  end Seq; "
        "  public static method Main(): int "
        "    var s := new Array<int>(); s.Add(10); s.Add(20); "
        "    var n: int := 0; "
        "    for var x in s do n := n + x; end; "
        "    var m: int := 0; "
        "    for var x in C.Seq() do m := m + x; end; "
        "    return n + m; "
        "  end Main; "
        "end C;";
    // n = 30 (10 + 20); m = 0 (empty Seq). Total 30.
    BEZEL_EXPECT_EQ(run(src), 30);
}

// ===========================================================================
// spec §28.9 / fix #2299 — dict-backed `Set<T>` RETURNED as IEnumerable<T>
// ===========================================================================

// Repro B — a dict-backed Set<int> returned as IEnumerable<int>. The
// snapshot via bz_lazy_from_set → bz_lazy_to_array is independent of the
// source set; the source set is freed immediately after the snapshot
// (the snapshot is what the wrap's slot-1 Dispose owns). Before the fix:
// 416B leak in 5 allocations (bz_dict_alloc_generic + dict_order_append +
// 3 key bytes) all in C.Seq. After the fix: leak-free. 5 + 6 = 11.
BEZEL_TEST(return_ienumerable_collection_leak_2299,
           local_set_returned_as_ienumerable_no_leak_spec_28_9) {
    std::string src =
        "module M; class C "
        "  public static method Seq(): IEnumerable<int> "
        "    var s := new Set<int>(); "
        "    s.Add(5); s.Add(6); "
        "    return s; "
        "  end Seq; "
        "  public static method Main(): int "
        "    var n: int := 0; "
        "    for var x in C.Seq() do n := n + x; end; "
        "    return n; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 11);
}

// ===========================================================================
// spec §28.9 / fix #2299 — REGRESSION the fix must NOT disturb
// ===========================================================================

// BORROWED source — a parameter (its owner is the caller; the RETURN does
// NOT take ownership). Pin: a `Seq(xs: Array<int>): IEnumerable<int> => xs`
// arrow-body passes the Array through unchanged. The wrap is the plain
// BORROWING enumerator; the caller's array is still alive (still in Main)
// and is disposed by Main's own scope-end reclaim. 5 + 6 = 11.
BEZEL_TEST(return_ienumerable_collection_leak_2299,
           borrowed_array_returned_still_borrows_spec_28_9) {
    std::string src =
        "module M; class C "
        "  public static method Seq(xs: Array<int>): IEnumerable<int> "
        "    return xs; "
        "  end Seq; "
        "  public static method Main(): int "
        "    using var s := new Array<int>(); "
        "    s.Add(5); s.Add(6); "
        "    var n: int := 0; "
        "    for var x in C.Seq(s) do n := n + x; end; "
        "    return n; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 11);
}

// LAZY pipeline — the OWNING-wrap sibling of #2299 (#1916). The pipeline
// is drained into a fresh owned Array<T> and the wrap's slot-1 Dispose
// frees it. Even before this fix the pipeline RETURN was leak-free — pin
// it so the OWNING-wrap shape change for Array doesn't accidentally
// regress the lazy path. {2, 3} → {2, 3} → sum 5.
BEZEL_TEST(return_ienumerable_collection_leak_2299,
           lazy_pipeline_return_still_leak_free_spec_5) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Seq(xs: Array<int>): IEnumerable<int> "
        "    return xs.Select(lambda(n) => n); "
        "  end Seq; "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [2, 3]; "
        "    var n: int := 0; "
        "    for var x in C.Seq(xs) do n := n + x; end; "
        "    return n; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 5);
}

// Arrow-body (=>) return — different code path through the same
// wrapAsIEnumerableForTarget plumbing; pin it for Array as well.
BEZEL_TEST(return_ienumerable_collection_leak_2299,
           arrow_body_array_returned_as_ienumerable_no_leak_spec_28_9) {
    std::string src =
        "module M; class C "
        "  public static method Seq(): IEnumerable<int> "
        "    var s := new Array<int>(); "
        "    s.Add(5); s.Add(6); "
        "    return s; "
        "  end Seq; "
        "  public static method Arrow(): IEnumerable<int> "
        "    => C.Seq(); "
        "  public static method Main(): int "
        "    var n: int := 0; "
        "    for var x in Arrow() do n := n + x; end; "
        "    return n; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 11);
}
