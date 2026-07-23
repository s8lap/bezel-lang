// Codegen/runtime — issue #2148 (§28.9 / §28.11, borrowing-collection reclaim in
// a SYNC iterator body): a plain `var` borrowing-`Array<T>` LITERAL — or a fresh
// borrowing-array / dict MATERIALIZER — declared inside a `yield` iterator body
// leaked its backing when the iterator ran to normal completion. `Array<T>` is
// trivial-dispose (its `Dispose()` is a no-op), but its backing storage is
// "reclaimed with the instance" (§28.9/§28.11) via a compiler-emitted scope-end
// bz_array_free, registered in lowerLocalDecl only when
// `currentFn_ == borrowingArrayFreeEligibleFn_` (and the dict/grouping analogues).
//
// ROOT CAUSE (src/codegen/LlvmBackend_Iterators.cpp, emitIteratorMethod). The
// MoveNext state-machine body lowers with currentFn_ == moveNextFn, but the
// iterator setup re-keyed ONLY unwindIneligibleFn_ to moveNextFn (the #1897 fix);
// borrowingArrayFreeEligibleFn_ / dictMaterializerFreeEligibleFn_ /
// groupingFreeEligibleFn_ stayed pinned to the ENTRY (factory) fn that
// emitFunctionBody established (Setup.cpp). So the `currentFn_ ==
// borrowingArrayFreeEligibleFn_` gate never matched inside MoveNext, every such
// reclaim was silently skipped, and the backing leaked on the iterator's normal
// completion (an LSan leak: 104 B for an `Array<int>` literal — 40 B spine +
// 64 B data).
//
// FIX. Re-establish the borrowing-array / dict / grouping reclaim eligibility
// keyed to moveNextFn in emitIteratorMethod, mirroring the async-resume-fn
// re-establishment landed for #2132 (LlvmBackend_Async.cpp) and the unwind
// re-establishment already present just above. One collectBorrowingArrayFree-
// Eligible call re-populates all three sets; all three fn keys re-pin to
// moveNextFn.
//
// OwningArray<T> (via `new` or via a `[...]` literal under a `using var`) is the
// CONTRAST: it is NOT in borrowingArrayFreeEligible_ (only name-head `Array` with
// a null element disposer is collected) — its disposal runs through the iterator's
// using-var machinery — so it stayed correct throughout and is kept as a green
// control.
//
// fail-without/pass-with. Without the fix each reclaim case leaks (LSan ->
// ran=false -> the -999 sentinel); with the fix every program is ASan/LSan-clean
// and returns the expected value. // per §28.9, §28.11 (issue #2148)

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// runAotChecked returns ran=false on any sanitizer fire (an LSan leak on the
// iterator's scope-end drop); map that to a sentinel so a single EXPECT_EQ pins
// BOTH memory-cleanliness AND the computed value.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

// For the owning-iterator follow-up cells: pin ran (link + ASan/LSan clean),
// exitCode, and the count of the parameter destructor's "dispose 1" line (must be
// exactly once — no double-dispose, no skipped dispose).
struct JitResultCount {
    bool ran;
    int  exitCode;
    int  disposeOnce;
};
JitResultCount runCount(const std::string& prog) {
    auto r = runAotChecked(prog);
    int n = 0;
    const std::string& s = r.stdoutText;
    const std::string needle = "dispose 1";
    for (std::size_t p = s.find(needle); p != std::string::npos;
         p = s.find(needle, p + needle.size()))
        ++n;
    return { r.ran, r.exitCode, n };
}
}  // namespace

// ── The core leak: a `var xs: Array<int> := [ … ]` LITERAL local in a `yield`
// body, held live ACROSS the yields, must have its backing reclaimed at the
// iterator's normal-completion scope end. Without the fix the MoveNext reclaim is
// skipped (borrowingArrayFreeEligibleFn_ still points at the factory fn) → 104 B
// leaks → -999. Yields 10, 20, 3 → total 33. // per §28.9, §28.11 (issue #2148)
BEZEL_TEST(iterator_borrowing_collection_reclaim_2148, array_literal_across_yields_reclaimed) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var xs: Array<int> := [10, 20, 30]; "
        "    yield return xs[0]; "
        "    yield return xs[1]; "
        "    yield return xs.Count; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var v in C.Gen() do total := total + v; end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        33);
}

// ── Yield-position axis: the literal is used ONLY BEFORE the first yield (never
// held across a suspend). The scope-end reclaim must still fire on normal
// completion regardless of use position. Yields 4 (=xs[0]+xs.Count), 100 → 104.
// per §28.9, §28.11 (issue #2148)
BEZEL_TEST(iterator_borrowing_collection_reclaim_2148, array_literal_used_before_first_yield_reclaimed) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    var s := xs[0] + xs.Count; "
        "    yield return s; "
        "    yield return 100; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var v in C.Gen() do total := total + v; end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        104);
}

// ── Element-category axis: a CLASS-element `Array<Dog>` literal that BORROWS its
// elements (the using-var dogs own them, disposed exactly once each). The backing
// reclaim uses a NULL per-element disposer (§28.11) and must NOT touch the dogs —
// a non-null disposer would double-free them (ASan → -999). Both dogs freed once
// by their using vars, backing freed once. Yields 2, 3, 4 → total 9.
// per §28.9, §28.11 (issue #2148)
BEZEL_TEST(iterator_borrowing_collection_reclaim_2148, class_element_array_literal_borrows_elements) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Dog public N: int := 0; "
        "  public constructor(x: int) N := x; end constructor; end Dog; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    using var d1: Dog := new Dog(3); "
        "    using var d2: Dog := new Dog(4); "
        "    var xs: Array<Dog> := [d1, d2]; "
        "    yield return xs.Count; "
        "    yield return xs[0].N; "
        "    yield return xs[1].N; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var v in C.Gen() do total := total + v; end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        9);
}

// ── STRING-DISPOSER axis: a `var parts := "…".Split(",")` — a fresh borrowing
// `Array<string>` MATERIALIZER whose scope-end reclaim carries a NON-null
// per-element disposer (the string-release thunk), unlike the int/class-borrow
// literal's null disposer. Its reclaim eligibility must be re-established for
// moveNextFn too, or the backing AND every substring leak on iterator completion.
// Used after a yield. Yields 1, 4 → total 5. // per §28.9, §28.11 (issue #2148)
BEZEL_TEST(iterator_borrowing_collection_reclaim_2148, string_split_materializer_reclaimed) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var parts := \"a,b,c,d\".Split(\",\"); "
        "    yield return 1; "
        "    yield return parts.Count; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var v in C.Gen() do total := total + v; end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        5);
}

// ── DICT-MATERIALIZER axis: a plain `var d := nums.ToDictionary(sel)` — a fresh
// `owned Dictionary<K,V>` materializer the local solely owns. Its scope-end
// reclaim is gated on dictMaterializerFreeEligibleFn_, co-re-established with the
// borrowing-array set by the same fix. Both the `nums` array literal AND the dict
// `d` must reclaim in the MoveNext body. Yields 1, 3 → total 4.
// per §28.9, §28.11 (issue #2148)
BEZEL_TEST(iterator_borrowing_collection_reclaim_2148, dict_materializer_reclaimed) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var nums: Array<int> := [1, 2, 3]; "
        "    var d := nums.ToDictionary(lambda(n: int) => n); "
        "    yield return 1; "
        "    yield return d.Count; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var v in C.Gen() do total := total + v; end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        4);
}

// ── SOUNDNESS (abandonment): the consumer BREAKS after the first element, so the
// iterator is Disposed while suspended (state 1) with the array literal live in
// its frame. The reclaim must fire exactly once on the abandonment/Dispose edge —
// no double-free (ASan) and no leak (LSan). Reads 10, then breaks → 10.
// per §28.9, §28.11 / §29.7 (issue #2148)
BEZEL_TEST(iterator_borrowing_collection_reclaim_2148, array_literal_reclaimed_on_abandonment) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    var xs: Array<int> := [10, 20, 30]; "
        "    yield return xs[0]; "
        "    yield return xs[1]; "
        "    yield return xs[2]; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var v in C.Gen() do "
        "      total := total + v; "
        "      if total >= 10 then break; end; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        10);
}

// ── REGRESSION GUARD (follow-up): the re-established reclaim eligibility re-runs
// collectBorrowingArrayFreeEligible during MoveNext lowering, which perturbed the
// order in which `bz_exc_addr` (the thread-exception-slot accessor) first entered
// the module. That symbol is declared BOTH by the owning-`TakeCurrent` error path
// (module_->getOrInsertFunction) and by emitExcSlot — and emitExcSlot used a plain
// llvm::Function::Create that does NOT dedupe, so when it ran second LLVM minted a
// colliding `bz_exc_addr.1` (undefined at link — the runtime exports only
// `bz_exc_addr`), breaking the LINK of a bare `yield return <owned param>` owning
// iterator. Fixed by deduping in emitExcSlot (getOrInsertFunction). This is the
// EXACT regressed shape (cf. iterator_owned_param_yield_transfer_1863): an
// IOwningEnumerable<R> iterator that yields its `owned` parameter, consumed via the
// manual owning protocol, must LINK, run, and dispose the parameter EXACTLY once.
// per §29.1, §29.8 (issue #2148 follow-up)
BEZEL_TEST(iterator_borrowing_collection_reclaim_2148, owning_iter_yield_owned_param_links_and_disposes_once) {
    JitResultCount r = runCount(
        "module M; "
        "class R "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor Console.WriteLine(\"dispose \" + Id.ToString()); end destructor; "
        "end R; "
        "class C "
        "  public static method Gen(owned a: R): IOwningEnumerable<R> "
        "    yield return a; "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var source := Gen(new R(1)); "
        "    using var en := source.GetOwningEnumerator(); "
        "    while en.MoveNext() do "
        "      using var u := en.TakeCurrent(); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);            // must LINK + run (no bz_exc_addr.1)
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.disposeOnce, 1);   // parameter disposed exactly once
}

// ── INTERACTION cell: an `owned` parameter yielded (transferred to the consumer,
// discharged at the yield) AND a borrowing-`Array<int>` LITERAL local reclaimed at
// MoveNext scope end, in the SAME iterator body. The param-transfer discharge and
// the array-backing reclaim must coexist: the parameter disposes exactly once (via
// the consumer), the array backing is freed once (no leak), no double-free.
// Value: u.Id = 1. // per §29.1, §29.8, §28.9, §28.11 (issue #2148 follow-up)
BEZEL_TEST(iterator_borrowing_collection_reclaim_2148, owning_iter_owned_param_and_array_literal_same_body) {
    JitResultCount r = runCount(
        "module M; "
        "class R "
        "  public Id: int := 0; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor Console.WriteLine(\"dispose \" + Id.ToString()); end destructor; "
        "end R; "
        "class C "
        "  public static method Gen(owned a: R): IOwningEnumerable<R> "
        "    var xs: Array<int> := [7, 8, 9]; "
        "    yield return a; "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var source := Gen(new R(1)); "
        "    using var en := source.GetOwningEnumerator(); "
        "    var sum := 0; "
        "    while en.MoveNext() do "
        "      using var u := en.TakeCurrent(); "
        "      sum := sum + u.Id; "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);            // links + ASan/LSan clean (array reclaimed)
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_EQ(r.disposeOnce, 1);   // parameter disposed exactly once
}

// ── CONTROL (the contrast, must stay green): an OwningArray<Box> built from a
// `[…]` literal under a `using var` in the iterator body. OwningArray is NOT in
// borrowingArrayFreeEligible_ (only name-head `Array` with a null element disposer
// is) — it disposes via the iterator's using-var machinery, correct before AND
// after this fix. Guards against the re-keying wrongly capturing an owning-family
// literal. Yields 2, 10 → total 12. // per §28.9 (issue #2148 contrast)
BEZEL_TEST(iterator_borrowing_collection_reclaim_2148, owning_array_literal_control) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Box public v: int := 0; "
        "  public constructor(x: int) v := x; end constructor; end Box; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    using var a: OwningArray<Box> := [new Box(10), new Box(20)]; "
        "    yield return a.Count; "
        "    yield return a[0].v; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var v in C.Gen() do total := total + v; end; "
        "    return total; "
        "  end Main; "
        "end C;"),
        12);
}
