// Spec-derived regression test for the SortedSet.Dispose() leak — narrow
// #2726 residue after the per-fix-review verdict on the broader fix.
//
// AUTHORITY. stdlib §4.18 (v0.278) declares on `SortedSet<T>`:
//
//     public class SortedSet<T>: IReadOnlyCollection<T> where T: IComparable<T>
//       ...
//       public method Clear();
//       ...
//     end SortedSet;
//
// stdlib §1 says "Every class implicitly implements `IDisposable`", so an
// explicit `s.Dispose()` on a `SortedSet<int>` must release the receiver.
//
// THE DEFECT. `src/codegen/LlvmBackend_Calls2.cpp:7178`'s `isSetName`
// predicate (the §28.2 explicit-Dispose classification gate that routes
// Set/OwningSet through `dictBackedSet` → `bz_dict_free(recv, null)`)
// matched only `{Set, OwningSet}`. A SortedSet receiver therefore fell
// past `dictBackedSet` and into the vtableless-BuiltinClass fall-through
// at line 7429 → `emitVtablelessBuiltinRelease("SortedSet", recv)`
// (Calls2.cpp:7483). `emitVtablelessBuiltinRelease`
// (Disposal.cpp:2198-2333) covers StringBuilder/Random/Atomic/
// AtomicInteger/EventSource/AsyncEventSource/TaskCompletionSource/
// OwningTaskCompletionSource/Channel/ChannelSender/Stopwatch/Mutex/Lock/
// Semaphore/SemaphoreToken/CancellationTokenSource/FileStream/Subscription
// but NOT SortedSet → returns false → emits NOTHING → bounded leak (the
// trade #2874/#2823 record). The SortedSet's bz_dict_t* allocation
// (19200 + 51200 bytes per instance = the dict plus its bucket array)
// was leaked.
//
// THE FIX. Add `SortedSet` to the `isSetName` headName match. The
// SortedSet runtime IS the bz_dict_t* with the SortedSet-comparable
// key ordering layered on (#1426); bz_dict_free disposes both the Set
// and SortedSet shapes equivalently.
//
// TEST. The exact #2967 shape (a `using var` + an explicit `.Dispose()`
// on the same receiver, 200 iterations) under generated-code ASan/LSan —
// the EXACT shape that surfaced the leak. The test fails without the
// fix (19200 + 51200 bytes leaked per iteration × 200 = 2.04 MiB total)
// and passes with the fix (no leak).
//
// Per stdlib §4.18 (v0.278), spec §28.2 (v0.135), fix #3360 (narrow
// SortedSet.Dispose() leak fix; broader #2726 thesis is unsolved and
// stays open).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

// The §28.2 shape under test: an explicit `.Dispose()` on a `using var`, so
// the explicit release (§28.2) and the §28.3 scope-exit auto-drop both reach
// one receiver. 200 iterations so a single-shot fluke cannot pass.
BEZEL_TEST(sorted_set_dispose_3360, sorted_set_dispose_does_not_leak_28_2) {
    auto r = runAotChecked(
        "module M; "
        "import System.Collections.Generic; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var s := new SortedSet<int>(); "
        "      s.Dispose(); "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}