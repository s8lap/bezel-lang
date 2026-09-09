// Codegen — issue #4092 (spec §29.2 *Iterator yield ownership*, §30.7
// *Asynchronous iterators*, v0.294): an `IAsyncEnumerable<T>` /
// `IAsyncEnumerator<T>` iterator returning a CLASS-TYPED element leaked every
// yielded `new C(...)` value when iteration ended — natural exhaustion, early
// `break`, or single-yield-with-exit. The §29.2 / §30.7 spec rule "the
// enumerator owns the most-recently-yielded value; the next MoveNext disposes
// the previous before producing the next, and Dispose disposes the
// outstanding yielded value" matches the SYNC iterator's §29 (#1120)
// `disposeYieldedVal` machinery (`LlvmBackend_Iterators.cpp`), but the ASYNC
// iterator never implemented it — `emitAsyncIteratorMethod` recognised only
// the OWNING head (`IOwningAsyncEnumerable<T>` / `IOwningAsyncEnumerator<T>`,
// fixed by #2591) and the BORROWING form's `current_val` trailing field was
// never disposed. The consumer's iteration variable `x` is a §29.2 BORROW
// and the compiler REFUSES `x.Dispose()` against it ("cannot call `Dispose()`
// on iteration variable … §29.2"), so the iterator is the SOLE party that
// can discharge — and it did not, leaking every yielded owned-class value.
//
// MEASURED on HEAD pre-fix (`origin/main 2d201b3`):
//   • main repro (3 yields × 50 iter): 2400 B / 150 allocs (16 B per leaked R).
//   • one-yield: 800 B / 50 allocs (per-yield, not per-iterator-frame).
//   • break-after-yield: 800 B / 50 allocs (only the yielded-then-borrowed
//     value leaks; un-yielded values are correctly abandoned).
//
// SPEC CITATIONS
//   §29.2 (v0.294) Iterator yield ownership: "The yield transfers ownership
//      to the iterator's enumerator. The enumerator owns the
//      most-recently-yielded value; the next MoveNext() disposes the previous
//      value before producing the next. … Disposing the enumerator (break,
//      exhaustion, or explicit Dispose()) disposes any outstanding yielded
//      value the iterator still owns."
//   §30.7 (v0.294) asynchronous iterators: "extends the same rule to
//      IAsyncEnumerable<T> and IAsyncEnumerator<T>."
//   §29.2 (v0.294) Iterator yield ownership: "The consumer's iteration
//      variable in `for var x in src do ... end` is a borrow of the
//      enumerator's owned slot."
//
// FIX SHAPE
//   Mirrors the sync §29 (#1120) machinery on the async frame:
//     • New `i8 current_val_owned` flag in the trailing frame fields (along-
//       side `current_val`, between it and `perAdvanceTask`).
//     • `elemIsOwnedClass` predicate in `emitAsyncIteratorMethod` (gated on
//       `!isOwningIter` and the element sema type being a Class or Interface
//       declaration — same authority the sync iterator uses,
//       `LlvmBackend_Iterators.cpp` line 1454).
//     • YieldStmt handler (`LlvmBackend_Stmts.cpp`) added an
//       `smKind_ == AsyncYield && iterElemIsOwnedClass_` arm that:
//         (a) disposes the PREVIOUS `current_val` iff `current_val_owned == 1`
//             (the `new C(...)` mint that survived the last yield),
//         (b) sets `current_val_owned = 1` iff the new value is a direct `new`
//             mint (and 0 for `yield return <local>` re-borrows).
//     • `disposeCurrentValOnExhaust` (Resume's exhaust dispose path — covers
//       the "Done" lambda, the fault path, AND the body-runs-off-its-end
//       path) extended to dispose-and-null on `current_val_owned == 1` for
//       the BORROWING class-element arm.
//     • `emitFreeAndReturn`'s `disposeYieldedVal` lambda (the iterator's
//       own Dispose) extended with the matching BORROWING arm.
//     • GetAsyncEnumerator's clone-byte-copy reset clears `current_val_owned`
//       on the clone so a re-iteration starts owned-free.
//     • All three dispose sites are dispose-then-null under the same null
//       guard, exactly-once across the yield-advance / exhaust / iterator-
//       Dispose triple — same invariant the sync #1120 fix established.
//
// ALL FOUR ROWS use `runAotChecked` (generated-code ASan + LSan). A leak
// (per #4092) fails the row before the exit value is compared.
//
// LOOP AMPLIFICATION (per [[lsan-one-shot-leak-tests-pass-vacuously]]): each
// row runs 500 iterations of the leak shape so a one-shot leak that happens
// to be reclaimed by an unrelated path cannot pass vacuously. 50 iterations
// alone produced 2400 B in the issue's main repro; 500 iterations produce
// ~24000 B, an order of magnitude past LSan noise floor. Matches the
// 500-iteration discipline every other codegen leak-pin in this tree uses
// (`lambda_arg_call_bounded_closure_release_4078_test.cpp`,
// `async_iterator_lambda_closure_3553_test.cpp`, etc.).
//
// ROW 4 (the owning twin control) MUST REMAIN CLEAN: IOwningAsyncEnumerable<T>
// was already implemented and disposing-from-the-consumer is the
// spec-sanctioned path; it stays that way. If the borrow-form fix
// accidentally engaged on the owning form (gating bug), the counter would
// go NEGATIVE (the iter would dispose AND the consumer would dispose, each
// yield's R would decrement _n twice); the row reads `R._n` which would be
// -1500 instead of 0. The `live + 1000` shim then returns -1500 + 1000 =
// -500 instead of the expected 0 — a distinctive non-zero exit that fails
// the row. The shim shape handles both "leak" (positive R._n) and "double-
// free" (negative R._n) in one symmetric oracle.
//
// per §29.2, §30.7 (v0.294)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// runAotChecked runs the program under generated-code ASan WITH LeakSanitizer
// on: a leak (the defect under test) fails the row before the printed exit
// value is compared; the exit value is itself an independent oracle on
// whether the iterator body and the consumer drove to the expected state.
int checkedExit(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

// Shared R: a class with a STATIC disposal counter; `R._n` is the number of
// destroyed R instances across the run. The same idiom as the sync #1120
// test (`iter_yield_owned_dispose_1120_test.cpp`'s `kCellDisposed`) and the
// async #2627 test (`async_enumerator_take_after_exhaustion_2627_test.cpp`'s
// `kCountR`); Mint + 1 on ctor, -1 on dtor so `_n` is net-live count.
const std::string kCounterR =
    "class R "
    "  public static _n: int := 0; "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; R._n := R._n + 1; end constructor; "
    "  destructor R._n := R._n - 1; end destructor; "
    "end R; ";

}  // namespace

// ── Row 1 (§29.2 / §30.7 issue repro HEADLINE): the iterator yields 3 owned
// values per iteration, the consumer iterates the for-loop to natural
// completion. Pre-fix: 2400 B / 150 allocs leaked per run; Post-fix:
// `R._n == 0` (every yielded R disposed exactly once, by the iterator on
// yield-advance AND on exhaust). Each iteration yields 3 Rs (500 × 3 = 1500
// total mints; 1500 expected disposals at end-of-run = 0 leaked; `R._n` ==
// 0 is the success signal). The shim maps any failure to a distinctive
// non-zero exit (1 from LSan differs from a clean 0; the shim is a guard
// against LSan reporting a clean number AND a leak at the same time — a
// defensive `additional` oracle).
//
// Expected: 0 (no leaks AND `R._n == 0` at end of run).
BEZEL_TEST(async_iter_borrowing_class_element_dispose_4092,
           three_yields_natural_exhaustion_disposes_each_exactly_once_29_2) {
    BEZEL_EXPECT_EQ(checkedExit(
        "module M; " + kCounterR +
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<R> "
        "    yield return new R(11); "
        "    yield return new R(22); "
        "    yield return new R(33); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 500 do "
        "      await for var x in Gen() do var t := x.tag; end; "
        "      i := i + 1; "
        "    end; "
        "    var live := R._n; "
        "    return (if live = 0 then 0 else live + 1000); "
        "  end Main; end C;"), 0);
}

// ── Row 2 (§29.2 one-yield sister repro): a single-yield body. Pre-fix:
// 800 B / 50 allocs leaked (one R per iter). Post-fix: R._n == 0. Same
// exhaust path as Row 1 but the yield-advance dispose branch never fires
// (only one yield, so no second yield to dispose the previous), so this
// isolates the EXHAUST-DISPOSE site from the YIELD-ADVANCE site.
//
// Expected: 0.
BEZEL_TEST(async_iter_borrowing_class_element_dispose_4092,
           one_yield_natural_exhaustion_disposes_outstanding_29_2) {
    BEZEL_EXPECT_EQ(checkedExit(
        "module M; " + kCounterR +
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<R> "
        "    yield return new R(11); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 500 do "
        "      await for var x in Gen() do var t := x.tag; end; "
        "      i := i + 1; "
        "    end; "
        "    var live := R._n; "
        "    return (if live = 0 then 0 else live + 1000); "
        "  end Main; end C;"), 0);
}

// ── Row 3 (§29.2 break-after-yield sister repro): consumer iterates the
// for-loop ONCE then `break`s. Pre-fix: 800 B / 50 allocs (only the
// yielded-then-borrowed value leaks — 2 of 3 yields were never reached,
// so the iterator's body never minted them, and the un-yielded ones
// have no leak source). Post-fix: R._n == 0 (the iterator's
// `iter.Dispose` disposes the outstanding `current_val` via the same
// `disposeYieldedVal` arm Row 1 uses on exhaustion, gated on the
// `current_val_owned` flag from the most-recent yield). Importantly,
// this tests the ABANDONMENT path — the iterator's `Dispose` is invoked
// from the for-loop's break edge with `current_val != null`,
// `current_val_owned == 1`.
//
// Expected: 0.
BEZEL_TEST(async_iter_borrowing_class_element_dispose_4092,
           break_after_first_yield_disposes_outstanding_29_2) {
    BEZEL_EXPECT_EQ(checkedExit(
        "module M; " + kCounterR +
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<R> "
        "    yield return new R(11); "
        "    yield return new R(22); "
        "    yield return new R(33); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 500 do "
        "      await for var x in Gen() do break; end; "
        "      i := i + 1; "
        "    end; "
        "    var live := R._n; "
        "    return (if live = 0 then 0 else live + 1000); "
        "  end Main; end C;"), 0);
}

// ── Row 4 (§29.2 *CONTROL* — owning twin must NOT regress): the
// IOwningAsyncEnumerable<R> control from the issue — consumer explicitly
// calls `x.Dispose()` (the owning contract transfers ownership at TakeCurrent),
// and the iterator MUST NOT also dispose. Pre-fix pre-#2591 it would have
// doubled (iterator disposed, then consumer disposed → double-free under
// ASan); post-#2591 it has been clean. Post-#4092 the borrow-form fix MUST
// NOT engage the owning arm (the new BORROWING arm of
// `emitFreeAndReturn`'s `disposeYieldedVal` is gated on
// `elemIsOwnedClass && !isOwningIter`, by construction mutually exclusive
// with `isOwningIter`). 500 iter × 3 yields = 1500 disposals expected;
// `R._n == 0` signals "every yielded R was disposed exactly once" (no
// double-free AND no leak).
//
// Expected: 0 (R._n == 0 — every yielded R was disposed exactly once, by
// the consumer's `x.Dispose()` inside the loop, NOT by the iterator).
BEZEL_TEST(async_iter_borrowing_class_element_dispose_4092,
           owning_twin_control_consumer_disposes_not_iterator_29_1) {
    BEZEL_EXPECT_EQ(checkedExit(
        "module M; " + kCounterR +
        "class C "
        "  public static async method Gen(): IOwningAsyncEnumerable<R> "
        "    yield return new R(11); "
        "    yield return new R(22); "
        "    yield return new R(33); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 500 do "
        "      await for var x in Gen() do var t := x.tag; x.Dispose(); end; "
        "      i := i + 1; "
        "    end; "
        "    var live := R._n; "
        "    return (if live = 0 then 0 else live + 1000); "
        "  end Main; end C;"), 0);
}
