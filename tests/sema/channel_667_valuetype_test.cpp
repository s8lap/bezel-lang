// fix #667 — §30.15 (v0.135) / stdlib §6.9: value-typed channel payloads,
// TryReceive-after-dispose, IsClosed-on-dispose.
//
// Three defects re-verified on cca9190:
//
// (a) UAF / null-deref: TryReceive (and IsClosed) after Channel.Dispose()
//     crashes (SIGSEGV / rc=139). Root cause: explicit Channel.Dispose() nulls
//     the `ch` slot; subsequent calls to IsClosed / TryReceive load null and
//     pass it to bz_channel_is_closed / bz_channel_try_receive, which lack
//     null-guards.  Design choice (§28.2 / §30.15): calling IsClosed or
//     TryReceive on a disposed Channel is NOT a checked-disposal fault —
//     §30.15 guarantees IsClosed is always observable ("dispose closes
//     immediately") and TryReceive has a defined false return for an
//     empty-or-closed channel (stdlib §6.9).  The fix: add null guards in
//     bz_channel_is_closed (null → 1 = closed) and bz_channel_try_receive
//     (null → 0 = empty).  The null represents "fully disposed and cleared"
//     — the same semantics as a closed-and-drained channel.
//
// (b) Value payload lost: Channel<int> Send(42) → TryReceive yields 0.
//     Root cause: TryReceive codegen (LlvmBackend_Calls.cpp) stored the raw
//     void* from bz_channel_try_receive directly into the out-slot regardless
//     of the slot's LLVM type.  For `int?` the out-slot is `{i8 hasValue; i32
//     value}`; storing a ptr there is a type mismatch — the i32 value field
//     remains 0.  Fix: when the out-slot is a struct type (nullable value type),
//     decode the void* via ptrtoint + truncate/bitcast to the inner type, then
//     build `{i8=1, innerVal}` and store.  For pointer types (class, string),
//     the existing store is correct.
//
// (c) IsClosed false after Channel.Dispose(): same null-guard defect as (a).
//     bz_channel_dispose already sets closed=true via channel_close_and_drain_
//     pending, but the null from the slot nulling in codegen means the runtime
//     never reads the flag.  The null→1 guard makes IsClosed return true.
//
// Spec derivation:
//   §30.15 (v0.135): "Disposing the Channel<T> itself closes it immediately."
//   stdlib §6.9: "TryReceive → false when the channel is empty or closed."
//   stdlib §6.9: "IsClosed → true after dispose or after last sender releases."

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ---------------------------------------------------------------------------
// (b) Value round-trip: Channel<int> — send 42, TryReceive → 42.
// §30.15 / stdlib §6.9: Send stores value; TryReceive delivers it.
// ---------------------------------------------------------------------------

// §30.15 / stdlib §6.9: basic Channel<int> value round-trip.
// Send(42) stores the integer payload; TryReceive(out got) delivers 42.
BEZEL_TEST(channel_667_valuetype,
           b1_channel_int_tryreceive_round_trip_value_payload_30_15) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        "    sender.Send(42); "
        "    var got: int? := null; "
        "    var ok := ch.TryReceive(out got); "
        "    if not ok then return 1; end; "
        "    if got = null then return 2; end; "
        "    return got!; "
        "  end Main; "
        "end Program;");
    // §30.15: Send stores 42; TryReceive delivers 42 into got.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §30.15 / stdlib §6.9: TryReceive distinguishes the 0-value from null/empty.
// If value == 0 and TryReceive returns true, got must be 0 (not null).
BEZEL_TEST(channel_667_valuetype,
           b2_channel_int_tryreceive_zero_value_disambiguation_30_15) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        "    sender.Send(0); "   // deliberately send 0
        "    var got: int? := null; "
        "    var ok := ch.TryReceive(out got); "
        "    if not ok then return 1; end; "   // must be true
        "    if got = null then return 2; end; " // must not be null
        "    if got! <> 0 then return 3; end; "  // must equal 0
        "    return 42; "
        "  end Main; "
        "end Program;");
    // §30.15 / stdlib §6.9: 0-value must be distinguishable from empty-channel null.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §30.15 / stdlib §6.9: TryReceive on empty channel returns false (got stays null).
BEZEL_TEST(channel_667_valuetype,
           b3_channel_int_tryreceive_empty_returns_false_30_15) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        // No Send — channel is empty.
        "    var got: int? := null; "
        "    var ok := ch.TryReceive(out got); "
        "    if ok then return 1; end; "       // must be false
        "    if got <> null then return 2; end; "  // got stays null
        "    return 42; "
        "  end Main; "
        "end Program;");
    // stdlib §6.9: TryReceive on empty channel → false, out = null.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §30.15: Channel<int> with a non-trivial value (99) to distinguish from zero-init.
BEZEL_TEST(channel_667_valuetype,
           b4_channel_int_tryreceive_nontrivial_value_99_30_15) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        "    sender.Send(99); "
        "    var got: int? := null; "
        "    ch.TryReceive(out got); "
        "    if got = null then return -1; end; "
        "    return got!; "
        "  end Main; "
        "end Program;");
    // §30.15: payload 99 delivered via TryReceive.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// ---------------------------------------------------------------------------
// (c) IsClosed truth table: §30.15 / stdlib §6.9.
// ---------------------------------------------------------------------------

// §30.15 / stdlib §6.9: IsClosed is false on a fresh open channel.
BEZEL_TEST(channel_667_valuetype,
           c1_channel_isclosed_false_fresh_open_30_15) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        "    if ch.IsClosed then return 1; end; "
        "    return 42; "
        "  end Main; "
        "end Program;");
    // stdlib §6.9: IsClosed = false while at least one sender is alive.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §30.15: IsClosed is true after the last ChannelSender is disposed.
BEZEL_TEST(channel_667_valuetype,
           c2_channel_isclosed_true_after_last_sender_dispose_30_15) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    var sender := ch.NewSender(); "
        "    sender.Dispose(); "   // last sender → closes channel
        "    if ch.IsClosed then return 42; end; "
        "    return 1; "
        "  end Main; "
        "end Program;");
    // §30.15: last-sender Dispose closes the channel → IsClosed=true.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §30.15: IsClosed is true after Channel.Dispose().
// This is the core defect (c): the null-guard in bz_channel_is_closed makes
// the null-slot call return 1.
BEZEL_TEST(channel_667_valuetype,
           c3_channel_isclosed_true_after_channel_dispose_30_15) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        "    sender.Send(42); "
        "    ch.Dispose(); "   // explicit channel dispose → IsClosed must be true
        "    if ch.IsClosed then return 42; end; "
        "    return 1; "
        "  end Main; "
        "end Program;");
    // §30.15: Channel.Dispose closes immediately → IsClosed=true.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ---------------------------------------------------------------------------
// (a) TryReceive after Channel.Dispose() — must not crash.
// §30.15 / stdlib §6.9: TryReceive on a closed (disposed) channel returns false.
// This is the UAF defect: the null-guard in bz_channel_try_receive fixes it.
// ---------------------------------------------------------------------------

// §30.15 / stdlib §6.9: TryReceive after Channel.Dispose returns false (no crash).
// Per design decision (§28.2 / §30.15): calling TryReceive on a disposed Channel
// is not a fault — it returns false (null = empty-and-closed).
BEZEL_TEST(channel_667_valuetype,
           a1_tryreceive_after_channel_dispose_returns_false_no_crash_30_15) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        "    sender.Send(42); "
        "    ch.Dispose(); "  // disposes + clears slot (null)
        // TryReceive on null (disposed) channel: must return false, not crash.
        "    var got: int? := null; "
        "    var ok := ch.TryReceive(out got); "
        "    if ok then return 1; end; "       // must be false
        "    return 42; "
        "  end Main; "
        "end Program;");
    // §30.15 / stdlib §6.9: TryReceive on disposed → false; no crash (fix (a)).
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §30.15: TryReceive after Channel.Dispose when channel was drained first.
// Confirms null-guard works even without items in the channel at dispose time.
BEZEL_TEST(channel_667_valuetype,
           a2_tryreceive_after_dispose_drained_channel_returns_false_30_15) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        // No items sent; dispose.
        "    ch.Dispose(); "
        "    var got: int? := null; "
        "    var ok := ch.TryReceive(out got); "
        "    if ok then return 1; end; "
        "    if got <> null then return 2; end; "
        "    return 42; "
        "  end Main; "
        "end Program;");
    // §30.15: disposed empty channel → TryReceive=false, got stays null.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §30.15 / stdlib §6.9: combined — send, then receive (TryReceive), then dispose,
// then TryReceive again. Both calls must work correctly.
BEZEL_TEST(channel_667_valuetype,
           a3_tryreceive_send_receive_then_dispose_then_false_30_15) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        "    sender.Send(7); "
        // First TryReceive: must return true with value 7.
        "    var got: int? := null; "
        "    var ok := ch.TryReceive(out got); "
        "    if not ok then return 1; end; "
        "    if got = null then return 2; end; "
        "    if got! <> 7 then return 3; end; "
        // Now dispose and try again.
        "    ch.Dispose(); "
        "    var got2: int? := null; "
        "    var ok2 := ch.TryReceive(out got2); "
        "    if ok2 then return 4; end; "   // must be false
        "    return 42; "
        "  end Main; "
        "end Program;");
    // §30.15: round-trip + post-dispose TryReceive=false.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ---------------------------------------------------------------------------
// Regression: #634 dispose permutations must still pass with the new fix.
// (These are a subset of channel_634 tests re-stated here to confirm no
// regression from the null-guard additions.)
// ---------------------------------------------------------------------------

// fix #634 regression: send-after-dispose still raises CCE (null guard must not
// silently swallow the error on the send path — send uses the non-nulled path
// because it goes through the ChannelSender which holds a core ref).
BEZEL_TEST(channel_667_valuetype,
           reg1_send_after_channel_dispose_still_raises_cce_30_15) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    var ch := new Channel<int>(); "
        "    var sender := ch.NewSender(); "
        "    ch.Dispose(); "
        "    var raised := false; "
        "    try sender.Send(1); "
        "    catch ChannelClosedException => raised := true; "
        "    end; "
        "    sender.Dispose(); "
        "    return (if raised then 42 else 91); "
        "  end Main; "
        "end Program;");
    // fix #634: send-after-dispose raises CCE; no regression.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// fix #634 regression: sender-then-channel dispose order (normal path).
BEZEL_TEST(channel_667_valuetype,
           reg2_senders_first_dispose_order_no_crash_30_15) {
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static method Main(): int "
        "    using var ch := new Channel<int>(); "
        "    using var sender := ch.NewSender(); "
        "    sender.Send(5); "
        "    sender.Dispose(); "   // last sender → closes channel
        // Channel is closed; TryReceive should deliver the buffered item.
        "    var got: int? := null; "
        "    var ok := ch.TryReceive(out got); "
        "    if not ok then return 1; end; "
        "    if got = null then return 2; end; "
        "    if got! <> 5 then return 3; end; "
        // Second TryReceive on drained closed channel → false.
        "    var got2: int? := null; "
        "    var ok2 := ch.TryReceive(out got2); "
        "    if ok2 then return 4; end; "
        "    return 42; "
        "  end Main; "
        "end Program;");
    // fix #634: normal sender-first close; TryReceive delivers item then false.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}
