// Codegen/ownership — OwningQueue.TryDequeue / OwningStack.TryPop with `out _`
// discards the `out owned` result, which must be disposed per §22.6 / §28.3
// (issue #5336, stdlib §4.11, v0.313).
//
// The builtin arms for TryDequeue / TryPop on OwningQueue / OwningStack write the
// owned element into the out slot. When the caller uses `out _` the slot is bound
// to nothing — §28.3's unbound-owned-temporary disposal was NOT being applied,
// causing a direct leak of the dequeued/popped element.
//
// THE FIX. After writing the element into the out slot, if `call.args[0].isDiscard`
// is true, call `disposeDiscardedOwnedOut` exactly as TryPopFirst/TryPopLast
// (§22.6 fix #5289) and TryPopValue/TryReceive/TryUpgrade do.
//
// Each test case uses a class R with a static-counting destructor so the
// LeakSanitizer leg verifies exactly-one disposal. runAotChecked also guards
// against a double-free the fix could introduce.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        // A toolchain skip and a real finding are DIFFERENT outcomes, and only
        // the former may be returned past. "runAotChecked: SKIP — C toolchain
        // (clang) ..." means the sanitizer never ran; "sanitizer fired" means it
        // ran and found something. Returning true for the second would discard
        // a genuine leak as if the box merely lacked clang — the #3733 silent
        // -skip defect, and the reason this predicate is not a bare find("SKIP").
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

// Count non-overlapping occurrences of `needle` in `hay`.
int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// ---------------------------------------------------------------------------
// OwningQueue.TryDequeue(out _) — the leaking pin (issue #5336).
// stdlib §4.11 (v0.313): `TryDequeue(out owned item: T?): bool`.
// Without the fix: `new R(1)` is dequeued but never disposed — direct leak.
// With the fix: "R-DISPOSED-1" appears exactly once.
// ---------------------------------------------------------------------------
const char* kOwningQueueTryDequeueDiscard =
    "module M; "
    "class R "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"R-DISPOSED-\" + Id.ToString()); "
    "  end destructor; "
    "end R; "
    "class C "
    "  public static method Take(b: bool, n: int): int return n; end Take; "
    "  public static method Main(): int "
    "    using var q := new OwningQueue<R>(); "
    "    q.Enqueue(move (new R(1))); "
    "    const seen := C.Take(q.TryDequeue(out _), 1); "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// Control: OwningQueue.TryDequeue(out owned var x) — real binding must still
// transfer ownership to x and dispose exactly once at scope end. Not a discard,
// so the discard path must not touch it.
// ---------------------------------------------------------------------------
const char* kOwningQueueTryDequeueBinding =
    "module M; "
    "class R "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"R-DISPOSED-\" + Id.ToString()); "
    "  end destructor; "
    "end R; "
    "class C "
    "  public static method Main(): int "
    "    using var q := new OwningQueue<R>(); "
    "    q.Enqueue(move (new R(2))); "
    "    var ok: bool := false; "
    "    ok := q.TryDequeue(out owned var x); "
    "    Console.WriteLine(\"ok=\" + ok.ToString()); "
    "    Console.WriteLine(\"id=\" + x!.Id.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// OwningStack.TryPop(out _) — same leak (issue #5336).
// stdlib §4.11 (v0.313): `TryPop(out owned item: T?): bool`.
// ---------------------------------------------------------------------------
const char* kOwningStackTryPopDiscard =
    "module M; "
    "class R "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"R-DISPOSED-\" + Id.ToString()); "
    "  end destructor; "
    "end R; "
    "class C "
    "  public static method Take(b: bool, n: int): int return n; end Take; "
    "  public static method Main(): int "
    "    using var s := new OwningStack<R>(); "
    "    s.Push(move (new R(3))); "
    "    const seen := C.Take(s.TryPop(out _), 1); "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// Control: OwningStack.TryPop(out owned var x) — real binding must still work.
// ---------------------------------------------------------------------------
const char* kOwningStackTryPopBinding =
    "module M; "
    "class R "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"R-DISPOSED-\" + Id.ToString()); "
    "  end destructor; "
    "end R; "
    "class C "
    "  public static method Main(): int "
    "    using var s := new OwningStack<R>(); "
    "    s.Push(move (new R(4))); "
    "    var ok: bool := false; "
    "    ok := s.TryPop(out owned var x); "
    "    Console.WriteLine(\"ok=\" + ok.ToString()); "
    "    Console.WriteLine(\"id=\" + x!.Id.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// #5336 — OwningQueue.TryDequeue(out _) leaks the dequeued owned element.
// Pin: without the fix "R-DISPOSED-1" never appears (direct leak of 16 bytes).
// =====================================================================
BEZEL_TEST(codegen_owning_queue_trydequeue_discard_leak_5336,
           owning_queue_trydequeue_out_underscore_leaks_spec_22_6_28_3) {
    // spec §22.6 (v0.313): against `out owned T` the discarded value is an
    // unbound owned temporary disposed at the end of the full statement (§28.3).
    JitResult r = runAotChecked(kOwningQueueTryDequeueDiscard);
    // #3733: a silent `return` here would report a clean PASS on a box
    // whose ASan link is unavailable — green while asserting nothing about
    // memory safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);

    // Load-bearing: the discarded owned R(1) WAS disposed exactly once.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "R-DISPOSED-1"), 1);
}

// =====================================================================
// Control: OwningQueue.TryDequeue(out owned var x) — ownership transfers to
// x; x is usable and auto-disposed at scope end. The discard fix must not
// interfere with this path.
// =====================================================================
BEZEL_TEST(codegen_owning_queue_trydequeue_discard_leak_5336,
           owning_queue_trydequeue_out_owned_var_binding_still_works) {
    JitResult r = runAotChecked(kOwningQueueTryDequeueBinding);
    // #3733: a silent `return` here would report a clean PASS on a box
    // whose ASan link is unavailable — green while asserting nothing about
    // memory safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);

    // x owns the dequeued value and it was disposed exactly once.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("ok=True") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("id=2") != std::string::npos);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "R-DISPOSED-2"), 1);
}

// =====================================================================
// #5336 — OwningStack.TryPop(out _) leaks the popped owned element.
// Pin: without the fix "R-DISPOSED-3" never appears.
// =====================================================================
BEZEL_TEST(codegen_owning_queue_trydequeue_discard_leak_5336,
           owning_stack_trypop_out_underscore_leaks_spec_22_6_28_3) {
    JitResult r = runAotChecked(kOwningStackTryPopDiscard);
    // #3733: a silent `return` here would report a clean PASS on a box
    // whose ASan link is unavailable — green while asserting nothing about
    // memory safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);

    // Load-bearing: the discarded owned R(3) WAS disposed exactly once.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "R-DISPOSED-3"), 1);
}

// =====================================================================
// Control: OwningStack.TryPop(out owned var x) — ownership transfers to
// x; x is usable and auto-disposed at scope end.
// =====================================================================
BEZEL_TEST(codegen_owning_queue_trydequeue_discard_leak_5336,
           owning_stack_trypop_out_owned_var_binding_still_works) {
    JitResult r = runAotChecked(kOwningStackTryPopBinding);
    // #3733: a silent `return` here would report a clean PASS on a box
    // whose ASan link is unavailable — green while asserting nothing about
    // memory safety. Invert the branch so the skip is loud, then skip.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (wasSkipped(r)) return;

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);

    BEZEL_EXPECT_TRUE(r.stdoutText.find("ok=True") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("id=4") != std::string::npos);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "R-DISPOSED-4"), 1);
}
