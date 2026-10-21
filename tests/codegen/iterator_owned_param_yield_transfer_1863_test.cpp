// Codegen / ownership — yielding an `owned` PARAMETER from an
// `IOwningEnumerable<T>` iterator must transfer ownership to the consumer, NOT
// double-dispose it. Issue #1863. Spec: §29.1 v0.196 (IOwningEnumerable /
// TakeCurrent — ownership of each yielded value transfers to the consumer),
// §29.8 (an iterator's undischarged `owned` parameter is auto-disposed by the
// frame at teardown).
//
// THE BUG (#1863). An `IOwningEnumerable<T>` iterator method with a bare
// `yield return a;` of an `owned` parameter `a` double-disposed it. The
// `yield return` transfers ownership of `a` into `current_val`; the consumer
// then takes it via `TakeCurrent()` (nulling `current_val`) and disposes it
// through its own `using var`. But the ownership pass (Ownership.cpp) treated a
// bare yielded identifier as a READ — leaving `a` Owned — so the §29.8
// owned-parameter auto-dispose ALSO listed `a` as never-discharged and disposed
// it a SECOND time at frame teardown (`disposeOwnedParams`,
// LlvmBackend_Iterators.cpp). Result: a double-free → the consumer freed the
// object, then the source frame's Dispose read/freed it again (heap-use-after-
// free, SIGSEGV / exit 139). bezelc even emitted the spurious §28.3/§29.8
// auto-drop must-use warning for `a`, the tell that sema believed `a` undischarged.
//
// THE FIX. In an OWNING iterator (return type IOwningEnumerable<T> /
// IOwningAsyncEnumerable<T>), a bare `yield return <owned identifier>` TRANSFERS
// ownership to the consumer, so it is DISCHARGED at the yield (like a bare
// `return x;` / `throw e;`) — `markDisposedByName` records it in
// `everDischarged_`, so the §29.8 auto-dispose skips it and only the consumer
// disposes it (exactly once). This does NOT change a NON-owning
// `IEnumerable<owned T>` iterator, whose enumerator RETAINS ownership of the
// yielded value (the consumer only borrows via `Current`) and MUST auto-dispose
// it at teardown.
//
// OBSERVABILITY. runAotChecked instruments the generated code with ASan/LSan, so
// WITHOUT the fix the double-free / use-after-free flips `r.ran` to false; WITH
// the fix the program runs and the parameter's destructor prints exactly once.

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

// The exact #1863 repro: an IOwningEnumerable<R> iterator that yields its `owned`
// parameter, consumed via the §29.1 manual owning protocol
// (source / GetOwningEnumerator / MoveNext / TakeCurrent). The single yielded R
// (Id 1) is adopted by a `using var` and must dispose EXACTLY once.
const char* kYieldOwnedParam =
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
    "end C;";

// A mix: an `owned` parameter yielded FIRST, then a fresh `new R()` yielded — so
// the discharge must be precise. The parameter (Id 1) is transferred to the
// consumer; the fresh allocation (Id 2) is enumerator-owned current_val that is
// likewise transferred at TakeCurrent. Both dispose EXACTLY once; neither the
// param's §29.8 auto-dispose nor the exhaustion cleanup may double-free.
const char* kYieldOwnedParamThenNew =
    "module M; "
    "class R "
    "  public static _seq: int := 0; "
    "  public Id: int := 0; "
    "  public constructor() R._seq := R._seq + 1; Id := R._seq; end constructor; "
    "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
    "end R; "
    "class C "
    "  public static method Gen(owned a: R): IOwningEnumerable<R> "
    "    yield return a; "
    "    yield return new R(); "
    "  end Gen; "
    "  public static method Main(): int "
    "    using var source := Gen(new R()); "   // Id 1
    "    using var en := source.GetOwningEnumerator(); "
    "    var sum := 0; "
    "    while en.MoveNext() do "
    "      using var u := en.TakeCurrent(); "
    "      sum := sum + u.Id; "
    "    end; "
    "    return sum; "   // Ids 1 + 2 = 3
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #1863 — yielding an owned parameter from an IOwningEnumerable<T> iterator
// disposes it EXACTLY once (no double-free). §29.1 v0.196 / §29.8.
// =====================================================================

BEZEL_TEST(iterator_owned_param_yield_transfer_1863,
           yield_owned_param_disposes_once_spec_29_1) {
    JitResult r = runAotChecked(kYieldOwnedParam);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);            // no double-free / use-after-free (ASan)
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // WITHOUT the fix the object was freed by the consumer then again by the
    // frame's §29.8 auto-dispose — so it printed once then crashed. WITH the fix
    // the parameter's destructor runs exactly once.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "dispose 1"), 1);
}

BEZEL_TEST(iterator_owned_param_yield_transfer_1863,
           yield_owned_param_then_new_each_once_spec_29_8) {
    JitResult r = runAotChecked(kYieldOwnedParamThenNew);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);            // LSan-clean + no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 3);      // Ids 1 + 2
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
}
