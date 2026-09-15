// Codegen / runtime — §30.15 / §28.2 (#4357): Channel.Dispose is idempotent.
//
// Root cause: bz_channel_dispose had no entry guard. Unlike bz_mutex_dispose /
// bz_semaphore_dispose (fix #706), it called channel_core_release on every
// invocation. A second Dispose() double-decremented core_refs, potentially
// freeing the channel while channel_close_and_drain_pending was still running.
//
// Fix (#4357): change bz_channel_t::disposed from bool to std::atomic<bool>
// and add if (ch->disposed.exchange(true)) return; at the top of
// bz_channel_dispose — the same pattern as fix #706.
//
// Test: call Dispose twice on a Channel. The second call must be a no-op
// (no crash, no double-free). Uses a class-typed item so the dispose path
// through reverse-item-disposal is exercised.  After tx.Dispose() (last sender
// closes the channel) the buffered items are still in the channel and are
// disposed by the first ch.Dispose(); the second ch.Dispose() is a no-op.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// A checked run is SKIPPED (not failed) when the ASan toolchain is absent:
// runAotChecked returns ran=false with a "SKIP" diagnostic. clang is required to
// build this repo, so a skip is not expected — guard anyway.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}
}  // namespace

// A sendable class item with a static destructor-counting oracle.
// Static field Item._n: ctor +1, dtor -1. After two sends + last-sender
// close + channel dispose, both items are in the buffer; they are drained
// and their dtors run (net _n change: 0→2→0). The second ch.Dispose()
// is a no-op.
const char* kItem =
    "sealed class Item "
    "  public V: int; "
    "  public constructor(v: int) Item._n := Item._n + 1; V := v; end constructor; "
    "  public static _n: int := 0; "
    "  destructor Item._n := Item._n - 1; end destructor; "
    "end Item; ";

// =====================================================================
// #4357 — functional: double-dispose is a no-op (exit code 42).
// Without the fix: double-decrement of core_refs → use-after-free / crash.
// With the fix: second dispose returns immediately, no crash.
// =====================================================================
BEZEL_TEST(channel_double_dispose_idempotent_4357,
           double_dispose_no_crash_spec_30_15) {
    JitResult r = runAot(
        std::string(
            "module M; import System.Threading; "
        ) + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<Item>(); "
        "    var tx := ch.NewSender(); "
        "    tx.Send(new Item(1)); "
        "    tx.Send(new Item(2)); "
        "    tx.Dispose(); "       // last sender closes the channel; items stay in buffer
        "    ch.Dispose(); "       // first dispose: drain + dispose items; release ref
        "    ch.Dispose(); "       // second dispose: no-op with the fix; crash without
        "    return 42; "          // if we get here, the double-dispose was harmless
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// #4357 — leak / ASan: double-dispose must not double-free. Without the
// fix core_refs is decremented twice → the channel is freed while the first
// dispose is still running. ASan / LSan catches the use-after-free or the
// double-free. With the fix the second call is a no-op.
// =====================================================================
BEZEL_TEST(channel_double_dispose_idempotent_4357,
           double_dispose_no_double_free_spec_30_15) {
    JitResult r = runAotChecked(
        std::string(
            "module M; import System.Threading; "
        ) + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<Item>(); "
        "    var tx := ch.NewSender(); "
        "    tx.Send(new Item(1)); "
        "    tx.Send(new Item(2)); "
        "    tx.Dispose(); "
        "    ch.Dispose(); "
        "    ch.Dispose(); "       // second dispose: no-op with the fix; crash without
        "    return 42; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;            // ASan toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(r.ran);             // no UAF / double-free
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
