// Codegen / runtime — §30.15 (v0.334) / stdlib §6.9 (#1807):
// `Channel<T>.Close()` — the receiver-initiated graceful close.
//
// THE GAP: stdlib §6.9 declares `public method Close();` and §30.15 states
// "The receiving end may close first-class: `Close()` puts the channel in the
// same closed state", but the compiler had NO arm for it:
//
//   repro.bzl:13:5: no member `Close` on `Channel<T>` (stdlib §6.9)
//
// Three layers were missing and all three are exercised here:
//
//   1. sema   — src/sema/ExprTypecheck_Calls.cpp: the Channel<T> member list
//               had Receive / TryReceive / NewSender / Count / IsClosed /
//               ReceiveAll and no `Close`, so the call was rejected at typecheck.
//   2. runtime— src/runtime/bezel_runtime_concurrency.cpp had no
//               `bz_channel_close` at all (only _new / _dispose / _count /
//               _is_closed / _new_sender / _receive / _receive_abandon /
//               _try_receive / _send / _send_async / _sender_* / _recv_all_*).
//   3. codegen— src/codegen/LlvmBackend_Calls3.cpp had no `Close` arm to emit
//               the call.
//
// THE POINT OF THE API — Close() vs Dispose() (§30.15, v0.334):
//
//   "The receiving end may close first-class: Close() puts the channel in the
//    same closed state — buffered items remain receivable, subsequent sends
//    fault with ChannelClosedException (disposing the offered item, the rule),
//    and a SendAsync pending at that instant has its item disposed by the
//    channel and its task faulted with ChannelClosedException ... Close() is
//    idempotent. Disposing the Channel<T> itself closes it immediately and
//    disposes every undelivered class-typed item in reverse insertion order."
//
// So Close() and Dispose() share the CLOSE STATE TRANSITION and differ
// sharply in DISPOSAL:
//
//   |                 | buffered items        | core ref released |
//   |-----------------+-----------------------+-------------------|
//   | Close()         | KEPT, still receivable| NO                |
//   | Dispose()       | disposed, reverse order| YES              |
//
// A Close() that disposed undelivered items would DOUBLE-FREE every buffered
// class-typed item when the channel is later disposed; a Close() that released
// the core ref would free the channel out from under a still-open, still-
// receivable channel (the #634 UAF). Rows 2 and 5 pin exactly this.
//
// ORACLES: runAot / runAotChecked only (the in-process JIT harness is
// deprecated). The leak row uses runAotChecked, whose LSan leg makes the
// dispose-exactly-once claim; runAotCheckedLenient sets detectLeaks=false and
// would make no such claim, so it is deliberately NOT used here. No row has an
// `if (wasSkipped(r)) return;` absorbing guard: on these accept rows that would
// turn a skipped leg into a silent pass, so `r.ran` is asserted directly and a
// leg that did not run FAILS the row.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A sendable class item with BOTH counters §30.15 needs to tell the two
// disposal postures apart:
//   `_n`       — NET LIVE count (+1 ctor, -1 dtor): distinguishes "disposed"
//                from "still owned by the channel" at any point.
//   `disposed` — cumulative destructor runs: pins EXACTLY ONCE (a second
//                disposal of the same object shows up here as an over-count).
// `sealed` so Item satisfies §30.14's sendable-class rule (Channel<Item>).
const char* kItem =
    "sealed class Item "
    "  public V: int; "
    "  public constructor(v: int) Item._n := Item._n + 1; V := v; end constructor; "
    "  public static _n: int := 0; "
    "  public static disposed: int := 0; "
    "  destructor Item._n := Item._n - 1; Item.disposed := Item.disposed + 1; "
    "    end destructor; "
    "end Item; ";

}  // namespace

// =====================================================================
// §30.15 v0.334 / stdlib §6.9 #1807 — Close() puts the channel in the
// closed state, and the channel REMAINS ALIVE (it is not a disposal):
// IsClosed reads true, and Count still reports the buffered item.
// Fail-without: the program is rejected at sema ("no member `Close`").
// =====================================================================
BEZEL_TEST(channel_close_1807, close_sets_closed_and_keeps_channel_alive_spec_30_15) {
    JitResult r = runAot(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(); "
        "    using var tx := ch.NewSender(); "
        "    tx.Send(9); "
        "    if ch.IsClosed then return 1; end; "       // open before Close
        "    ch.Close(); "
        "    if not ch.IsClosed then return 2; end; "   // closed after Close
        "    if ch.Count <> 1 then return 3; end; "    // item still buffered
        "    return 42; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// THE DISPOSE-SEMANTICS ROW (stdlib §6.9 "Closing" / §30.15 v0.334):
// "Close() ... buffered items remain receivable".  So Close() must NOT
// dispose them: `Item._n` is still 2 immediately after Close(), the two
// items are still receivable afterwards in FIFO order, and each is
// released exactly once by the receiver.
//
// This is the row that fails if Close() is implemented as disposal —
// `Item._n` would read 0 right after Close() (row exits 20) instead of 2.
// Fail-without: rejected at sema.
// =====================================================================
BEZEL_TEST(channel_close_1807,
           close_does_not_dispose_buffered_items_and_they_stay_receivable_spec_6_9) {
    JitResult r = runAot(
        std::string(
            "module M; import System.Threading; "
        ) + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<Item>(); "
        "    using var tx := ch.NewSender(); "
        "    tx.Send(new Item(1)); "
        "    tx.Send(new Item(2)); "
        "    ch.Close(); "
        "    if Item._n <> 2 then return 20; end; "     // Close disposed NOTHING
        "    var got1 := await ch.Receive(); "          // still receivable after Close
        "    var got2 := await ch.Receive(); "
        "    var sum := got1.V * 10 + got2.V; "         // 12 — FIFO preserved
        "    got1.Dispose(); "
        "    got2.Dispose(); "
        "    if sum <> 12 then return 30; end; "
        "    if Item._n <> 0 then return 31; end; "     // both released
        "    if Item.disposed <> 2 then return 32; end; " // EXACTLY once each
        "    return 42; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// §30.15 v0.334: "Close() is idempotent: on an already-closed channel it
// is a no-op".  Three spellings of the already-closed precondition are
// exercised, so an implementation that only tolerates one of them fails:
//
//   (a) Close() twice in a row (plain repeat);
//   (b) Close() AFTER the last sender was disposed (the channel is
//       already closed by the sender path — "later sender disposals
//       remain no-ops with respect to the closed state");
//   (c) Close() after a Receive has drained the buffer.
//
// In every case the channel still drains correctly and every item is
// disposed exactly once (Item.disposed == 2).
// Fail-without: rejected at sema.
// =====================================================================
BEZEL_TEST(channel_close_1807, close_is_idempotent_spec_30_15) {
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
        "    ch.Close(); "
        "    ch.Close(); "                            // (a) plain repeat
        "    tx.Dispose(); "                           // (b) later sender disposal
        "    ch.Close(); "                            //     no-op on a closed channel
        "    if not ch.IsClosed then return 10; end; "
        "    var got1 := await ch.Receive(); "         // (c) buffered items survive
        "    var got2 := await ch.Receive(); "
        "    got1.Dispose(); "
        "    got2.Dispose(); "
        "    if Item.disposed <> 2 then return 20; end; "
        "    ch.Close(); "                            // still a no-op when drained
        "    if not ch.IsClosed then return 30; end; "
        "    return 42; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// stdlib §6.9 "Closing" (v0.334): "subsequent `Send`/`SendAsync` raise
// `ChannelClosedException` (disposing the offered item first)" — and "a
// channel never retains an item it did not accept".  So a Send on a
// Close()d channel faults AND the offered item is disposed exactly once
// (Item.disposed == 1, net-live back to 0).
// Fail-without: rejected at sema.
// =====================================================================
BEZEL_TEST(channel_close_1807,
           send_after_close_faults_and_disposes_offered_item_spec_6_9) {
    JitResult r = runAot(
        std::string(
            "module M; import System.Threading; "
        ) + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var ch := new Channel<Item>(); "
        "    var tx := ch.NewSender(); "
        "    ch.Close(); "
        "    var caught := 0; "
        "    try "
        "      tx.Send(new Item(9)); "
        "    catch ChannelClosedException => do caught := 1; end "
        "    end; "
        "    if caught <> 1 then return 10; end; "        // must fault
        "    if Item._n <> 0 then return 20; end; "        // offered item disposed
        "    if Item.disposed <> 1 then return 30; end; "  // exactly once
        "    ch.Dispose(); "
        "    tx.Dispose(); "
        "    return 42; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// THE LEAK ROW (§30.15 v0.334 / §28.11 owned column / §28.2
// dispose-exactly-once).  200 iterations x 3 class-typed items = 600
// Items, each of which the CHANNEL owns after Send.
//
// Three distinct defects each fail this row, which is why it loops:
//   * Close() that disposed undelivered items  -> 600 DOUBLE-FREES (ASan)
//   * Close() that leaked undelivered items     -> 600 LEAKS (LSan)
//   * Close() that released the core ref       -> UAF on the next iteration
// The in-program `Item._n == 0` assertion is the reader-trustworthy
// oracle — a skipped leg cannot fake it.
//
// LOOP AMPLIFICATION: 200x3 = 600 leaked objects is well past LSan noise;
// a one-shot leak repro is vacuous.
// Fail-without: rejected at sema (the whole program fails to compile).
// =====================================================================
BEZEL_TEST(channel_close_1807,
           close_then_dispose_releases_every_buffered_item_once_no_leak_spec_30_15) {
    JitResult r = runAotChecked(
        std::string(
            "module M; import System.Threading; "
        ) + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var ch := new Channel<Item>(); "
        "      using var tx := ch.NewSender(); "
        "      tx.Send(new Item(1)); "
        "      tx.Send(new Item(2)); "
        "      tx.Send(new Item(3)); "
        "      ch.Close(); "        // close WITHOUT disposing the 3 buffered items
        "      i := i + 1; "
        "    end; "                // <- each iteration disposes ch (and tx) here
        "    if Item._n <> 0 then return 70; end; "
        "    if Item.disposed <> 600 then return 71; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    // No `if (wasSkipped(r)) return;` guard: on an accept row that absorbing
    // site turns a skipped ASan leg into a silent pass. Assert ran directly.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// THE DIFFERENTIAL CONTROL (stdlib §6.9 "Closing" / §30.15 v0.334):
// the SAME setup WITHOUT Close() must still DISPOSE the undelivered
// items when the channel itself is disposed — "Disposing the Channel<T>
// itself closes it immediately and disposes every undelivered class-typed
// item in reverse insertion order".
//
// This is what keeps the Close arm honest: an accept row that only ever
// fires for one carrier is not a fix. If Close() were (wrongly) wired to
// the dispose path, the two rows would still agree on Item.disposed == 2
// but this one pins that Dispose DOES drain and Close does NOT (row 2).
// Fail-without: this row PASSES pre-fix (it is the control).
// =====================================================================
BEZEL_TEST(channel_close_1807,
           dispose_still_drains_undelivered_items_control_spec_30_15) {
    JitResult r = runAot(
        std::string(
            "module M; import System.Threading; "
        ) + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var ch := new Channel<Item>(); "
        "    var tx := ch.NewSender(); "
        "    tx.Send(new Item(1)); "
        "    tx.Send(new Item(2)); "
        "    ch.Dispose(); "        // disposes BOTH buffered items (reverse order)
        "    if Item._n <> 0 then return 10; end; "
        "    if Item.disposed <> 2 then return 20; end; "
        "    if not ch.IsClosed then return 30; end; "   // dispose closes it
        "    tx.Dispose(); "
        "    return 42; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}