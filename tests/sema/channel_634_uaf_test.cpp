// fix #634 — §30.15 (v0.135): Channel core refcounting / UAF regression.
//
// Root cause: bz_channel_dispose deleted the bz_channel_t core while live
// bz_channel_sender_t objects still held raw pointers to it.  Any subsequent
// Send via those senders dereferenced freed memory (mutex UAF → hang/crash).
//
// Fix (fix #634): bz_channel_t carries an atomic ref-count (core_refs).
//   - Channel object holds +1 (bz_channel_new, initial value 1).
//   - Each ChannelSender holds +1 (bz_channel_new_sender).
//   - Channel.Dispose drops its +1 via channel_core_release; the core is
//     freed only when the last ref (channel or sender) releases.
//   - ChannelSender.Dispose drops its +1 via channel_core_release.
//   - bz_channel_send / bz_channel_sender_send now take cce_vtable; when
//     the channel is closed/disposed they store a ChannelClosedException in
//     bz_exc and return (codegen checks bz_exc via emitPendingCheck).
//
// Spec derivation:
//   §30.15 (v0.135): "Disposing the Channel<T> itself closes it immediately
//     and disposes every undelivered class-typed item in reverse insertion
//     order; subsequent sends raise ChannelClosedException."
//
// Test shapes (4 probes + dispose-permutation lifecycle tests):
//   P1: send-after-channel-dispose — expect ChannelClosedException.
//   P2: receive-after-channel-dispose — expect ChannelClosedException.
//   P3: sender outlives channel in another domain — sender.Send → exception.
//   P4: dispose-channel-with-items-in-flight — items disposed, no crash.
//   L1: dispose permutation: channel-then-senders.
//   L2: dispose permutation: senders-then-channel.
//   L3: dispose permutation: interleaved (partial sender release before channel).

#include "support/codegen_support.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiagContaining(const bezel::test::SemaResult& r, const std::string& substr) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find(substr) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// P1: send-after-channel-dispose — expect ChannelClosedException (§30.15).
// Channel.Dispose closes the channel immediately; the live ChannelSender then
// calls Send — must raise ChannelClosedException (not hang/crash/UAF).
// ---------------------------------------------------------------------------

// §30.15: "Disposing the Channel<T> itself closes it immediately ...;
// subsequent sends raise ChannelClosedException."
BEZEL_TEST(channel_634_uaf, p1_send_after_channel_dispose_raises_cce_30_15) {
    auto r = runJit(
        "module M; "
        "sealed class Msg "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "  destructor end destructor; "
        "end Msg; "
        "class Program "
        "  public static method Main(): int "
        "    var ch := new Channel<Msg>(); "
        "    var tx := ch.NewSender(); "
        // Dispose the channel while the sender is still live.
        "    ch.Dispose(); "
        // Send after channel disposed must raise ChannelClosedException.
        "    var raised := false; "
        "    try "
        "      tx.Send(new Msg(1)); "
        "    catch ChannelClosedException => raised := true; "
        "    end; "
        "    tx.Dispose(); "
        // Return 42 if the exception was raised (fix verified), 91 if not (bug).
        "    return (if raised then 42 else 91); "
        "  end Main; "
        "end Program;");
    // §30.15: send after Channel.Dispose must raise ChannelClosedException.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ---------------------------------------------------------------------------
// P2: two senders — send-after-channel-dispose raises CCE on both senders.
// After Channel.Dispose, ALL live senders see a closed channel; each Send
// raises ChannelClosedException independently. Verifies that the fix #634
// core_refs accounting is correct for multiple senders.
// ---------------------------------------------------------------------------

// fix #634 / §30.15: with two senders, both should raise CCE after ch.Dispose().
BEZEL_TEST(channel_634_uaf, p2_two_senders_send_after_channel_dispose_cce_30_15) {
    auto r = runJit(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    var ch := new Channel<int>(); "
        "    var tx1 := ch.NewSender(); "
        "    var tx2 := ch.NewSender(); "
        // Send one item before dispose (buffered).
        "    tx1.Send(10); "
        // Dispose the channel; items are discarded, channel is closed.
        "    ch.Dispose(); "
        // Both senders should now raise CCE.
        "    var r1 := false; "
        "    var r2 := false; "
        "    try tx1.Send(11); catch ChannelClosedException => r1 := true; end; "
        "    try tx2.Send(22); catch ChannelClosedException => r2 := true; end; "
        "    tx1.Dispose(); "
        "    tx2.Dispose(); "
        "    return (if (r1 and r2) then 42 else 91); "
        "  end Main; "
        "end Program;");
    // fix #634: both senders see closed channel, raise CCE; no crash.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ---------------------------------------------------------------------------
// P3: sender outlives channel in another domain.
// Spawn a domain that holds a ChannelSender; dispose the Channel in the main
// domain; the spawned sender then sends — must raise ChannelClosedException,
// no crash/UAF.
// ---------------------------------------------------------------------------

// §30.15 / §30.14: the ChannelSender ref-counts the channel core (fix #634);
// after Channel.Dispose the core is NOT freed while the sender is alive.
// The sender's Send raises ChannelClosedException via bz_exc.
BEZEL_TEST(channel_634_uaf, p3_sender_outlives_channel_domain_send_raises_cce_30_15) {
    auto r = runJit(
        "module M; "
        "sealed class Token "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "  destructor end destructor; "
        "end Token; "
        // Worker: receives a ChannelSender, sends one item, catches CCE.
        "class Worker "
        "  public static method TrySend(owned tx: ChannelSender<Token>): int "
        "    using var sender := tx; "
        "    var raised := false; "
        "    try "
        "      sender.Send(new Token(7)); "
        "    catch ChannelClosedException => raised := true; "
        "    end; "
        "    return (if raised then 1 else 0); "
        "  end TrySend; "
        "end Worker; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    var ch := new Channel<Token>(); "
        "    var tx := ch.NewSender(); "
        // Dispose the channel BEFORE spawning so the sender sees a disposed channel.
        "    ch.Dispose(); "
        // Spawn: sender is now the only live ref to the core (fix #634 refcount).
        "    using var t := Domain.Spawn(Worker.TrySend, owned tx); "
        "    using var result := await t; "
        "    return result; "
        "  end Main; "
        "end Program;");
    // §30.15: sender sees closed channel, raises CCE, no UAF.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ---------------------------------------------------------------------------
// P4: dispose channel with items in flight — items disposed, no crash.
// Channel.Dispose must dispose undelivered items in REVERSE insertion order
// (§28.11) while the channel core may still have live senders.
// ---------------------------------------------------------------------------

// §28.11 / §30.15: undelivered items are disposed in reverse insertion order
// by Channel.Dispose.  After dispose the core is still accessible to senders
// (they hold a ref), but the items are gone and sends raise CCE.
BEZEL_TEST(channel_634_uaf, p4_dispose_channel_with_items_flushes_in_reverse_30_15) {
    auto r = runJit(
        "module M; "
        "sealed class Counter "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "  destructor end destructor; "
        "end Counter; "
        "class Program "
        "  public static method Main(): int "
        // Create channel and send 3 items without receiving them.
        "    var ch := new Channel<Counter>(); "
        "    var tx := ch.NewSender(); "
        "    tx.Send(new Counter(10)); "
        "    tx.Send(new Counter(20)); "
        "    tx.Send(new Counter(30)); "
        // Dispose: items 30, 20, 10 are disposed in reverse order (§28.11).
        "    ch.Dispose(); "
        // After dispose, sends must raise CCE.
        "    var raised := false; "
        "    try "
        "      tx.Send(new Counter(99)); "
        "    catch ChannelClosedException => raised := true; "
        "    end; "
        "    tx.Dispose(); "
        "    return (if raised then 42 else 91); "
        "  end Main; "
        "end Program;");
    // §28.11 / §30.15: items disposed; subsequent send raises CCE.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ---------------------------------------------------------------------------
// L1: dispose permutation: channel-then-senders.
// Channel is disposed before all senders — core freed when last sender releases.
// ---------------------------------------------------------------------------

// fix #634 refcount lifecycle: channel-first dispose. After Channel.Dispose
// the core has refs = (sender count); core is freed when the last sender releases.
BEZEL_TEST(channel_634_uaf, l1_channel_dispose_before_senders_no_crash_30_15) {
    auto r = runJit(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    var ch := new Channel<int>(); "
        "    var tx1 := ch.NewSender(); "
        "    var tx2 := ch.NewSender(); "
        // Dispose channel first.
        "    ch.Dispose(); "
        // Both senders still valid (core alive via their refs).
        // Sends raise CCE.
        "    var r1 := false; "
        "    var r2 := false; "
        "    try tx1.Send(1); catch ChannelClosedException => r1 := true; end; "
        "    try tx2.Send(2); catch ChannelClosedException => r2 := true; end; "
        // Release senders — last release frees core.
        "    tx1.Dispose(); "
        "    tx2.Dispose(); "
        "    return (if (r1 and r2) then 42 else 91); "
        "  end Main; "
        "end Program;");
    // fix #634: core freed after last sender; no crash.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ---------------------------------------------------------------------------
// L2: dispose permutation: senders-then-channel.
// All senders disposed before Channel.Dispose — normal path (closes via sender).
// fix #634: core freed by channel_core_release in bz_channel_dispose (sole ref).
// ---------------------------------------------------------------------------

// §30.15: last-sender dispose closes the channel; Channel.Dispose afterwards
// is idempotent (channel already closed and disposed).
BEZEL_TEST(channel_634_uaf, l2_senders_before_channel_dispose_no_crash_30_15) {
    auto r = runJit(
        "module M; "
        "sealed class Item "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "  destructor end destructor; "
        "end Item; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<Item>(); "
        "    using var tx := ch.NewSender(); "
        "    tx.Send(new Item(3)); "
        "    tx.Dispose(); "   // last sender → closes channel
        // Channel is now closed; can still drain.
        "    using var item := await ch.Receive(); "   // item 3
        // Next receive on drained+closed raises CCE.
        "    try "
        "      using var _ := await ch.Receive(); "
        "      return 91; "
        "    catch ChannelClosedException => return item.Val; "   // 3
        "    end; "
        "  end Main; "
        "end Program;");
    // §30.15: drain then CCE; senders-first dispose is the normal path.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ---------------------------------------------------------------------------
// L3: dispose permutation: interleaved (one of two senders released before
// channel dispose, then channel disposed, then second sender released).
// ---------------------------------------------------------------------------

// fix #634 refcount lifecycle: interleaved dispose order.
// refs at peak: 3 (ch + tx1 + tx2); after tx1.Dispose = 2; after ch.Dispose = 1;
// after tx2.Dispose = 0 → core freed.
BEZEL_TEST(channel_634_uaf, l3_interleaved_dispose_no_crash_30_15) {
    auto r = runJit(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    var ch := new Channel<int>(); "
        "    var tx1 := ch.NewSender(); "
        "    var tx2 := ch.NewSender(); "
        // Release first sender.
        "    tx1.Dispose(); "
        // Dispose channel (core_refs: 3 → 2 after tx1; 2 → 1 after ch.Dispose).
        "    ch.Dispose(); "
        // Second sender still valid (core alive via its ref).
        "    var raised := false; "
        "    try tx2.Send(5); catch ChannelClosedException => raised := true; end; "
        // Release second sender — last release; core_refs 1 → 0 → freed.
        "    tx2.Dispose(); "
        "    return (if raised then 42 else 91); "
        "  end Main; "
        "end Program;");
    // fix #634: core freed exactly once; no double-free or UAF.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ---------------------------------------------------------------------------
// #4318 — stdlib §30.15 v0.315: Channel<T> and ChannelSender<T> are
// null-declaration builtins that skip their own where T: sendable constraint.
// Before fix: no constraint check fired. Fix adds kNullDeclSendableConstraints
// to TypeResolution_Names.cpp so the sendable constraint is checked.
// ---------------------------------------------------------------------------

// §30.15 / #4318: Channel<NC> where NC is a non-sendable class — rejected
// with "does not satisfy the constraint `where T: sendable`".
BEZEL_TEST(channel_634_uaf,
           channel_nonsendable_element_rejected_sendable_constraint_s4318_001) {
    auto r = parseSemaAll(
        "module M; "
        "class NC end NC; "
        "class Program "
        "  public static method Main(): int "
        "    var ch := new Channel<NC>(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagContaining(r, "does not satisfy the constraint `where T: sendable`"));
}

// Control: Channel<int> — int is sendable, compiles cleanly (warning only
// from auto-dispose which is expected).
BEZEL_TEST(channel_634_uaf,
           channel_int_element_accepted_sendable_constraint_s4318_002) {
    auto r = runJit(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
