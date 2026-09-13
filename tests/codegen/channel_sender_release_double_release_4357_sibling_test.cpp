// Codegen / runtime — §30.15 / §28.2 (#4357 sibling): bz_channel_sender_release
// is idempotent against a double release.
//
// Root cause: bz_channel_sender_release had no entry guard. The function read
// tx->ch, then `delete tx`, then decremented ch->sender_count, then closed
// the channel if it was the last sender, then dropped the sender's core_refs.
// A SECOND arrival at the same tx pointer would (a) dereference tx->ch from
// freed memory, (b) decrement sender_count on a freed/reused channel,
// (c) call channel_close_and_drain_pending / channel_core_release and
// double-free the channel core. Identical defect class to #4357 channel
// dispose (fixed on r18-c7 as 16c22c7) and the filestream sibling (r18-c7
// e2a6e5a): a dispose/release path enters twice and the second entry has
// no guard.
//
// Fix (#4357 sibling): add `std::atomic<bool> released{false}` to
// bz_channel_sender_t and add `if (tx->released.exchange(true)) return;` at
// the top of bz_channel_sender_release — the same pattern as fix #706 /
// #4357.
//
// Test: call Dispose twice on a ChannelSender. The second call must be a
// no-op (no crash, no double-free). Uses a class-typed item so the channel
// close path (drain + dispose buffered items via bz_channel_dispose) is
// exercised. Loops 20× so the allocator is more likely to hand the same
// chunk back and the freed-struct reads become observably bad memory
// rather than zeroed-out memory that happens to be safe.
//
// Note (defense-in-depth): today's codegen nulls the slot between explicit
// Dispose() calls and null-guards the using-var scope-end emit, so the
// standard pattern `tx.Dispose(); tx.Dispose();` is short-circuited by the
// slot-null at the call site — the second call sees rdi=0 and the runtime's
// `if (!txp) return;` early-out fires. The runtime fix is nevertheless the
// right defense-in-depth: any future codegen change that stops nulling the
// slot, or a future caller that bypasses the slot (e.g. a runtime helper
// holding the raw pointer across a domain boundary), will land on a runtime
// that does the right thing. This mirrors the rationale the channel sibling
// (#4357) and the filestream sibling fix carry.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// A checked run is SKIPPED (not failed) when the ASan toolchain is absent:
// runAotChecked returns ran=false with a "SKIP" diagnostic. clang is required
// to build this repo, so a skip is not expected — guard anyway.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// A sendable class item (sealed, value fields) so Channel<R> is well-formed
// (§30.14). The Item type is local to this translation unit — anonymous
// namespace gives it internal linkage so it cannot collide with the same
// name in sibling tests (#4357 channel dispose also defines a `kItem`).
const char* kItem =
    "sealed class Item "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Item; ";
}  // namespace

// Module header + class shell + Main body + close. Built around the body so
// the Bezel source can contain `";` (inside `end C;`) without prematurely
// closing a C++ raw-string literal.
std::string wrapMain(const std::string& body) {
    return std::string(
        "module M; import System.Threading; "
    ) + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
    + body +
        "  end Main; "
        "end C;";
}

// Body: one ChannelSender double-dispose to exercise the basic
// last-sender-closes-channel path, then a 20-iteration loop where each
// iteration mints a fresh channel + sender, sends two items, and disposes
// the sender twice. The fresh-channel-per-iteration avoids the
// "channel already closed by previous tx.Dispose()" path so each iteration
// exercises the actual sender double-release path. The second dispose in
// each iteration must be a no-op (the runtime fix); without the runtime
// fix a misuse-driven second release would UAF the tx struct and
// double-free the channel core.
const std::string kBody =
    "    using var ch := new Channel<Item>(); "
    "    var tx := ch.NewSender(); "
    "    tx.Send(new Item(1)); "
    "    tx.Send(new Item(2)); "
    "    tx.Dispose(); "       // first release: last sender, channel closes
    "    tx.Dispose(); "       // second release: no-op with the fix; UAF without
    "    var i := 0; "
    "    while i < 20 do "     // amplify: ASan exit code shifts more reliably
    "      using var ch2 := new Channel<Item>(); "
    "      var t := ch2.NewSender(); "
    "      t.Send(new Item(3)); "
    "      t.Send(new Item(4)); "
    "      t.Dispose(); "      // first release: last sender, channel closes
    "      t.Dispose(); "      // second release: still a no-op with the fix
    "      i := i + 1; "
    "    end; "
    "    return 42; ";         // if we get here, the double-release was harmless

// =====================================================================
// #4357 sibling — functional: double-release of a sender is a no-op (exit
// code 42). Without the fix: second release reads freed memory and double-
// frees the channel core. With the fix: second release returns immediately,
// no crash.
// =====================================================================
BEZEL_TEST(channel_sender_release_double_release_4357_sibling,
           double_release_no_crash_spec_30_15) {
    JitResult r = runAot(wrapMain(kBody));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// #4357 sibling — leak / ASan: double-release must not UAF or double-free.
// Without the fix the second release dereferences tx->ch from freed memory
// (UAF) and calls channel_close_and_drain_pending / channel_core_release on
// a freed channel core (double-free). ASan / LSan catches one or the other.
// With the fix the second release is a no-op.
// =====================================================================
BEZEL_TEST(channel_sender_release_double_release_4357_sibling,
           double_release_no_uaf_no_double_free_spec_30_15) {
    JitResult r = runAotChecked(wrapMain(kBody));
    if (wasSkipped(r)) return;            // ASan toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(r.ran);             // no UAF / double-free
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}