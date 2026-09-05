// Codegen / leak — issue #3706 (§28.9 / §28.11 / §29.2 "Iterator yield
// ownership" preamble, v0.294): `yield return xs;` — a BARE READ of a plain
// `var xs: Array<int> := [1, 2, 3];` — from a BORROWING iterator (returning
// `IEnumerable<Array<int>>` / `IAsyncEnumerable<Array<int>>`) leaked the
// array's whole backing, in BOTH sync and async iterators.
//
//     public static method Gen(): IEnumerable<Array<int>>
//       var xs: Array<int> := [1, 2, 3];
//       yield return xs;
//     end Gen;
//
//     $ bezelc --sanitize=address -o p p.bzl && ./p   # 200 full drains
//     Direct leak of 11088 byte(s) in 198 object(s) ... bz_array_alloc
//     SUMMARY: AddressSanitizer: 23880 byte(s) leaked in 398 allocation(s).
//     (async twin: 24000 B / 400 allocations, exact match to the issue.)
//
// §29.2's "Iterator yield ownership" preamble (spec v0.294) rules this an
// implementation gap, not an open question: "a borrowing-family collection
// literal ... bound to a local of the body ... [yielded] in an iterator
// returning IEnumerable<Array<int>>: the iterator frame owns the literal, an
// owned local of the body released at its scope end under §28.3's ordinary
// rules, and the consumer borrows it." The escape walk
// (`collectBorrowingArrayFreeEligible`'s `StringEscapeWalker`,
// LlvmBackend_Setup.cpp) correctly declined to arm the scope-end
// `bz_array_free` (a `yield return xs;` was an unconditional escape) — the
// SAFE direction — but nothing else ever reclaimed the backing, hence the
// leak.
//
// THE FIX. A new OPT-IN walker flag (`yieldIsBorrowIterYield`) treats a
// `yield return <bare tracked local>;` as a borrow-position read (like the
// pre-existing `for var x in a` relaxation), so the local stays eligible for
// its ordinary scope-end reclaim — set ONLY by `emitIteratorMethod` /
// `emitAsyncIteratorMethod` (LlvmBackend_Iterators.cpp /
// LlvmBackend_Async.cpp) when the enclosing iterator's declared return type
// is a BORROWING enumeration interface (`!isOwningIter`, the SAME flag the
// TakeCurrent/owning-contract logic already reads — cannot disagree with it).
//
// ⚠ NEVER TRADE A LEAK FOR A DOUBLE-FREE. For an OWNING iterator
// (`IOwningEnumerable<T>`) the yield operand is instead a K3.7 CONSUME
// (moved-from) — ownership transfers to the consumer via `TakeCurrent()`, a
// completely different discharge story — so `yieldIsBorrowIterYield` stays
// false there and the pre-existing conservative-escape treatment (a leak,
// the safe direction) is UNCHANGED. Row 6 below is the regression guard: the
// owning-iterator control's PRINTED VALUE must stay correct and UNCHANGED by
// this fix (this fix does not touch that code path — `isOwningIter` gates it
// out at the two call sites). Row 6 was on the UNCHECKED oracle (`runAot`)
// when this file was written: its own `TakeCurrent()`-bound consumer-side
// local (`const arr := en.TakeCurrent();`) tripped a SEPARATE, PRE-EXISTING
// leak, filed as #3878 and discovered while writing this row, that
// reproduced byte-identically with this fix applied and with it fully
// reverted. #3878 has SINCE landed (a new `isFreshTakeCurrentBuiltinArrayLocal`
// recognizer joining the `collectBorrowingArrayFreeEligible` family,
// `LlvmBackend_Setup.cpp`), so Row 6 is now on `runAotChecked` like every
// other row — see its own comment for the updated cross-check.
//
// All rows are `runAotChecked` (generated-code ASan with LeakSanitizer on),
// loop-amplified to 200 activations: a one-shot row reads clean on BOTH
// sides (the single allocation is still stack-reachable at exit), so it
// would prove nothing. per §28.9, §28.11, §29.2 (issue #3706)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with
// LeakSanitizer on, so a leak, a double free, or a use-after-free fails the
// row (ran=false) before the printed value is even compared.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── Row 1 (SYNC) — the issue's repro: a bare `Array<int>` literal local
// yielded whole from a borrowing (`IEnumerable<T>`) sync iterator, drained
// fully 200 times. Pre-fix: ~23880 B / 398 allocations leaked (`bz_array_alloc`).
// 200 * 3 = 600. per §28.9, §28.11, §29.2 (issue #3706)
BEZEL_TEST(iterator_yield_borrowing_array_literal_reclaim_3706,
           sync_bare_array_literal_yield_reclaimed_spec_29_2) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IEnumerable<Array<int>> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      for var arr in C.Gen() do total := total + arr.Count; end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}

// ── Row 2 (ASYNC) — the async-iterator twin. Pre-fix: exactly 24000 B / 400
// allocations leaked (the issue's own measured number). 200 * 3 = 600.
// per §28.9, §28.11, §29.2, §30.7 (issue #3706)
BEZEL_TEST(iterator_yield_borrowing_array_literal_reclaim_3706,
           async_bare_array_literal_yield_reclaimed_spec_30_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "import System.Threading.Tasks; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<Array<int>> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      await for var arr in C.Gen() do total := total + arr.Count; end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}

// ── Row 3 (SYNC, SOUNDNESS) — abandonment: the consumer `break`s after the
// first of THREE yields, so the iterator is Disposed while suspended with
// `xs` still live in its frame. §29.7's disposal pass must reclaim it
// exactly once (no leak, no double-free) — the same route #2148's control
// row already exercises for a non-yielded literal. 200 * 3 = 600 (one
// element read per drain, three per Gen() call). per §28.9, §28.11, §29.2,
// §29.7 (issue #3706)
BEZEL_TEST(iterator_yield_borrowing_array_literal_reclaim_3706,
           sync_bare_array_literal_yield_reclaimed_on_abandonment_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IEnumerable<Array<int>> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    yield return xs; "
        "    yield return xs; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      for var arr in C.Gen() do "
        "        total := total + arr.Count; "
        "        break; "
        "      end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}

// ── Row 4 (ASYNC, SOUNDNESS) — the async twin of row 3.
// per §28.9, §28.11, §29.2, §29.7, §30.7 (issue #3706)
BEZEL_TEST(iterator_yield_borrowing_array_literal_reclaim_3706,
           async_bare_array_literal_yield_reclaimed_on_abandonment_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "import System.Threading.Tasks; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<Array<int>> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    yield return xs; "
        "    yield return xs; "
        "    yield return xs; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var j in 0..<200 do "
        "      await for var arr in C.Gen() do "
        "        total := total + arr.Count; "
        "        break; "
        "      end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}

// ── Row 5 (SYNC, SOUNDNESS) — an exception thrown from the consumer body
// right after reading the first yielded value forces the enumerator's
// implicit unwind-Dispose while `xs` is still live in the suspended frame,
// exactly as an early `break` does (row 3), but via the exception edge
// instead. 200 exceptions thrown and caught, one per Gen() drain.
// per §28.9, §28.11, §29.2, §29.7, §31.2 (issue #3706)
BEZEL_TEST(iterator_yield_borrowing_array_literal_reclaim_3706,
           sync_bare_array_literal_yield_reclaimed_on_throw_spec_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IEnumerable<Array<int>> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    yield return xs; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method Drain(): int "
        "    var total := 0; "
        "    for var arr in C.Gen() do "
        "      total := total + arr.Count; "
        "      throw new Exception(\"boom\"); "
        "    end; "
        "    return total; "
        "  end Drain; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    for var i in 0..<200 do "
        "      try "
        "        Drain(); "
        "      catch "
        "        Exception => do caught := caught + 1; end, "
        "      end try; "
        "    end; "
        "    Console.WriteLine(\"caught=\" + caught.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "caught=200\n");
}

// ── Row 6 (OWNING CONTROL, must stay clean and UNCHANGED by this fix) — the
// SAME bare-array-literal yield, but from an OWNING iterator
// (`IOwningEnumerable<Array<int>>`): per §29.2's owning-iterator bullet the
// yield operand is a K3.7 CONSUME (moved-from), a different discharge story
// this fix does not touch (`isOwningIter` gates `yieldIsBorrowIterYield`
// false at both call sites). Guards against a future change accidentally
// widening the relaxation to the owning path, which the ruling's own text
// warns would double-free (ownership already transferred to the consumer
// via `TakeCurrent()`). 200 * 3 = 600.
//
// #3878 FIXED (checked oracle restored): when this file was written, `arr`
// (bound via `const arr := en.TakeCurrent();`) tripped a SEPARATE,
// pre-existing leak (issue #3878) unrelated to this fix — neither
// `isBorrowingArrayLiteralLocal` nor `isFreshBorrowingArrayMaterializerLocal`
// (both require a `CollectionLiteralExpr` initializer) ever added the
// `CallExpr`-initialized `arr` to a reclaim-eligibility set. That leak
// reproduced byte-identically (24000 B / 400 allocations) both with this
// #3706 fix applied and with it fully reverted, confirming #3706 neither
// caused nor cured it — `Gen()`'s own `yield return xs;` is correctly
// UNAFFECTED here (`isOwningIter` is true, so `yieldIsBorrowIterYield` is
// false, so this program's `MoveNext` lowers identically before and after
// #3706). #3878 has SINCE landed (a new `isFreshTakeCurrentBuiltinArrayLocal`
// recognizer joining this same `collectBorrowingArrayFreeEligible` family),
// so this row is back on the checked oracle like every other row — it now
// cross-checks BOTH fixes at once: the owning path's printed value stays
// correct and untouched by #3706 (unchanged property), AND `arr`'s backing
// is now reclaimed (the #3878 property), so a regression in EITHER fix
// fails this row.
BEZEL_TEST(iterator_yield_borrowing_array_literal_reclaim_3706,
           owning_iterator_bare_array_literal_yield_unaffected_control) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<Array<int>> "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    yield return xs; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 0..<200 do "
        "      using var source := C.Gen(); "
        "      using var en := source.GetOwningEnumerator(); "
        "      while en.MoveNext() do "
        "        const arr := en.TakeCurrent(); "
        "        total := total + arr.Count; "
        "      end; "
        "    end; "
        "    Console.WriteLine(\"total=\" + total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=600\n");
}
