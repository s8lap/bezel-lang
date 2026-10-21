// Codegen / leak — a `() -> owned T` closure passed to a SYNC ITERATOR that
// returns `IOwningEnumerable<T>` is captured into the iterator's state-machine
// FRAME, which must invoke it across every MoveNext. That closure cell was never
// released: the caller conservatively treats a closure passed as a call argument
// as escaping (so it suppresses its own scope-end release, transferring
// responsibility to the callee — LlvmBackend_Setup.cpp ClosureEscapeWalker), yet
// the frame neither retained nor released it. The cell (40 bytes) therefore
// leaked. Issue #1864 (split off from #1837, which fixed a distinct generic-
// instantiation ICmp crash). Spec: language spec §29.2 (for-loop / owning-
// enumerable desugaring — the enumerator/source frames), §28.5 (owned-field /
// capture disposal cascade), §27.5/§28.9 (closure cell reference counting),
// stdlib §4.13 (IOwningEnumerable / IOwningEnumerator, v0.196).
//
// THE BUG (#1864). `emitIteratorMethod` stored the `make` closure parameter into
// the frame as a bare borrowed pointer and its Dispose freed the frame, the
// spilled for-loop area, and any owned-class param — but never the closure cell.
// The leak is ALWAYS present, but LSan (a reachability detector) only reports it
// when a trailing statement after the consumption loop reuses the register that
// still held the (now-dead) closure pointer: `Console.WriteLine("sum=" +
// sum.ToString())` does, so the exact-once disposal of the elements (D1/D2) is
// unaffected while the closure frame silently leaks. Sibling #1837's instantiated
// test returns straight out of the loop (no trailer), so it never surfaced this.
//
// THE FIX. Model the closure cell with the runtime ARC already used for closure
// FIELD-STORES: the iterator frame RETAINS its own +1 on each function-typed
// capture at construction (emitIteratorMethod) and RELEASES it at the SOURCE
// Dispose (emitDisposeBody's releaseClosureCaptures, disposeOwned=true only — the
// per-loop clone borrows the capture and must not release it). The caller then
// drops its transient producer reference at the call site (lowerCall's sync-
// iterator ARGUMENT-PASS release, the closure analogue of the #429 string one).
// Net: the closure cell is freed exactly once, no matter how many enumerator
// clones the source spawns, and no element is double-freed.
//
// OBSERVABILITY. runAotChecked links + runs under ASan/LSan; a leaked closure
// cell or a double-free/UAF flips r.ran to false. The trailing statement is
// REQUIRED for the without-fix leak to be LSan-visible, so it is part of every
// repro here.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Count non-overlapping occurrences of `needle` in `hay`.
int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// The exact #1864 repro: a CONCRETE (non-generic) `IOwningEnumerable<R>` iterator
// taking a `() -> owned R` factory closure, drained across TWO elements via the
// §29.1 manual owning protocol, followed by a TRAILING statement (a string-concat
// WriteLine) that surfaces the otherwise-masked closure-frame leak. Each yielded R
// is adopted by a `using var` and disposed exactly once (destructor prints
// "D<Id>"); the source + enumerator frames AND the captured closure cell must all
// be freed. Sum of Ids 1 + 2 = 3.
const char* kConcreteTrailingStmt =
    "module M; "
    "class R "
    "  public static _seq: int := 0; "
    "  public Id: int := 0; "
    "  public constructor() R._seq := R._seq + 1; Id := R._seq; end constructor; "
    "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
    "end R; "
    "class C "
    "  public static method Gen(make: () -> owned R): IOwningEnumerable<R> "
    "    yield return make(); "
    "    yield return make(); "
    "  end Gen; "
    "  public static method Main(): int "
    "    using var source := Gen(lambda() => new R()); "
    "    using var en := source.GetOwningEnumerator(); "
    "    var sum := 0; "
    "    while en.MoveNext() do "
    "      using var u := en.TakeCurrent(); "
    "      sum := sum + u.Id; "
    "    end; "
    "    Console.WriteLine(\"sum=\" + sum.ToString()); "   // trailing stmt — surfaces the leak
    "    return sum; "                                     // 1 + 2 = 3
    "  end Main; "
    "end C;";

// The #1641 temporary-source variant: `Gen(() => ...).GetOwningEnumerator()`
// returns the source frame ITSELF (no clone), whose full Dispose (disposeOwned=
// true) both frees the frame and — with the fix — releases the captured closure.
// A trailing statement again makes the without-fix closure leak LSan-visible.
const char* kTemporarySourceTrailingStmt =
    "module M; "
    "class R "
    "  public static _seq: int := 0; "
    "  public Id: int := 0; "
    "  public constructor() R._seq := R._seq + 1; Id := R._seq; end constructor; "
    "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
    "end R; "
    "class C "
    "  public static method Gen(make: () -> owned R): IOwningEnumerable<R> "
    "    yield return make(); "
    "    yield return make(); "
    "  end Gen; "
    "  public static method Main(): int "
    "    using var en := Gen(lambda() => new R()).GetOwningEnumerator(); "
    "    var sum := 0; "
    "    while en.MoveNext() do "
    "      using var u := en.TakeCurrent(); "
    "      sum := sum + u.Id; "
    "    end; "
    "    Console.WriteLine(\"sum=\" + sum.ToString()); "
    "    return sum; "                                     // 1 + 2 = 3
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #1864 — multi-element IOwningEnumerable consumption + a trailing statement is
// LSan-clean: the captured factory closure cell is freed exactly once, and each
// yielded element is disposed exactly once. language spec §29.2 / §28.5, stdlib
// §4.13. runAotChecked = ASan/LSan oracle (a leaked cell flips r.ran).
// =====================================================================

BEZEL_TEST(owning_enumerable_closure_capture_leak_1864,
           concrete_multi_element_trailing_stmt_lsan_clean_spec_29_2) {
    JitResult r = runAotChecked(kConcreteTrailingStmt);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);            // compiled + LSan-clean (closure cell freed)
    BEZEL_EXPECT_EQ(r.exitCode, 3);      // 1 + 2
    // Each yielded element disposed exactly once — a double-free would also flip
    // r.ran; these pin the exact-once count positively.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
}

BEZEL_TEST(owning_enumerable_closure_capture_leak_1864,
           temporary_source_trailing_stmt_lsan_clean_spec_29_1) {
    JitResult r = runAotChecked(kTemporarySourceTrailingStmt);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
}
