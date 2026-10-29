// Spec-derived ownership tests for issues #692 and #693.
//
// #692 — §6.9 / §30.15: ChannelSender<T>.Send ownership carve-out for
// value-typed / string T.
//
//   §30.15 (stdlib §6.9): "For sendable value-typed T and string, the
//   ownership modifiers drop out: Send(item: T)."  The channel slot is a
//   copy — there is no heap object to transfer and the owned-sink rule must
//   NOT fire. builtinOwnedSinkArg was unconditionally marking Send argPos 0
//   as an owned sink regardless of T, causing a false "borrow cannot be
//   passed where owned is required" error for int/bool/string/enum locals.
//
// #693 — §30.14: Domain.Spawn sinks ALL entry arguments (argPos >= 1).
//
//   §30.14: "Each extra argument is owned-transferred into the spawned
//   domain; the entry is responsible for disposing them."  builtinOwnedSinkArg
//   had `argPos == 1`, so only the first extra arg was an owned sink. A
//   second owned arg (e.g. ch.NewSender() at pos 2) was falsely rejected
//   with "borrow cannot be passed where owned is required".

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

// ---------------------------------------------------------------------------
// #692 — ChannelSender<int>.Send carve-out (§6.9 / §30.15)
// ---------------------------------------------------------------------------

// §30.15: Send(item: int) — value-typed T → ownership modifiers drop out.
// Passing a plain local `n: int` must be accepted (no owned sink, no borrow
// rejection). This is the exact repro from issue #692.
BEZEL_TEST(channel_spawn_sink_692_693,
           channel_send_int_local_accepted_6_9_30_15) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class W "
        "  public static method P(owned tx: ChannelSender<int>) "
        "    using var s := move tx; "
        "    var n := 41; "
        "    s.Send(n); "
        "    s.Send(42); "
        "  end P; "
        "end W;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.15: Send(item: bool) — bool is also value-typed; same carve-out.
BEZEL_TEST(channel_spawn_sink_692_693,
           channel_send_bool_local_accepted_6_9_30_15) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class W "
        "  public static method P(owned tx: ChannelSender<bool>) "
        "    using var s := move tx; "
        "    var flag := true; "
        "    s.Send(flag); "
        "  end P; "
        "end W;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.15: Send(item: string) — string is runtime-managed, also drops out.
BEZEL_TEST(channel_spawn_sink_692_693,
           channel_send_string_local_accepted_6_9_30_15) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class W "
        "  public static method P(owned tx: ChannelSender<string>) "
        "    using var s := move tx; "
        "    var msg := \"hello\"; "
        "    s.Send(msg); "
        "  end P; "
        "end W;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.15 NEGATIVE: ChannelSender<T> with class-typed T — owned sink DOES apply.
// A borrow of a class-typed local must be rejected at Send (the channel takes
// ownership of the heap object). This pins the negative boundary so the
// carve-out is not over-applied to class channels.
BEZEL_TEST(channel_spawn_sink_692_693,
           channel_send_class_borrow_rejected_6_9_30_15) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Report end Report; "
        "class W "
        "  public static method P(b: Report, owned tx: ChannelSender<Report>) "
        "    using var s := move tx; "
        "    s.Send(b); "  // b is a borrow → must be rejected
        "  end P; "
        "end W;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// §30.15 POSITIVE boundary: ChannelSender<T> with class-typed T and a `new`
// expression (an owned value) must still be accepted.
//
// `Report` is `sealed` so the element type argument is SENDABLE: §30.14 makes
// a class type sendable iff it is transfer-closed (`sealed`), so an unsealed
// `Report` is now rejected at the `ChannelSender<Report>` type argument with
// §14.2's `does not satisfy the constraint where T: sendable` — a DIFFERENT
// rule from the one this row pins. Sealing leaves the row's subject intact:
// T is still a class type, `new Report()` is still an owned value, and the
// boundary still tested is owned-vs-borrow at the `Send` call site.
BEZEL_TEST(channel_spawn_sink_692_693,
           channel_send_class_new_expr_accepted_6_9_30_15) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "sealed class Report end Report; "
        "class W "
        "  public static method P(owned tx: ChannelSender<Report>) "
        "    using var s := move tx; "
        "    s.Send(new Report()); "  // new → owned value → accepted
        "  end P; "
        "end W;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// #693 — Domain.Spawn sinks ALL entry args (argPos >= 1), per §30.14
// ---------------------------------------------------------------------------

// §30.14: Domain.Spawn(entry, owned arg1, owned arg2) — owned-returning calls
// at BOTH pos 1 and pos 2 must be accepted. This is the headline case for #693:
// ch.NewSender() (an owned-producing call) at pos 2 was falsely rejected with
// "borrow cannot be passed where owned is required" because builtinOwnedSinkArg
// only returned true for argPos == 1.
//
// Shape: Worker.Run(owned tx1: ChannelSender<int>, owned tx2: ChannelSender<int>)
// Domain.Spawn(Worker.Run, ch1.NewSender(), ch2.NewSender())
//   pos 1: ch1.NewSender() → owned ChannelSender<int>  [was already accepted]
//   pos 2: ch2.NewSender() → owned ChannelSender<int>  [was falsely rejected]
BEZEL_TEST(channel_spawn_sink_692_693,
           spawn_two_owned_args_both_accepted_30_14) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Worker "
        "  public static method Run("
        "      owned tx1: ChannelSender<int>, "
        "      owned tx2: ChannelSender<int>) "
        "    using var s1 := move tx1; "
        "    using var s2 := move tx2; "
        "    s1.Send(1); "
        "    s2.Send(2); "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var ch1 := new Channel<int>(); "
        "    using var ch2 := new Channel<int>(); "
        "    using var w := Domain.Spawn(Worker.Run, ch1.NewSender(), ch2.NewSender()); "
        "    await w; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14: pos 1 alone still accepted (regression guard for the original
// behaviour that already worked before #693 fix).
BEZEL_TEST(channel_spawn_sink_692_693,
           spawn_one_owned_arg_pos1_accepted_30_14) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Worker "
        "  public static method Run(owned tx: ChannelSender<int>) "
        "    using var s := move tx; "
        "    s.Send(1); "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(); "
        "    using var w := Domain.Spawn(Worker.Run, ch.NewSender()); "
        "    await w; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14 NEGATIVE: a borrow at a Spawn arg position (pos 1) must still be
// rejected. The owned-sink rule does not permit passing a bare borrow.
BEZEL_TEST(channel_spawn_sink_692_693,
           spawn_borrow_at_pos1_rejected_30_14) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Worker "
        "  public static method Run(owned tx: ChannelSender<int>) "
        "    using var s := move tx; "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main(b: ChannelSender<int>): Task<int> "
        "    using var w := Domain.Spawn(Worker.Run, b); "  // b is a borrow → rejected
        "    await w; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// §30.14 NEGATIVE: a borrow at pos 2 must also be rejected. This pins the
// lower bound — argPos >= 1 means every extra arg is an owned sink, so a
// borrow at pos 2 should be diagnosed just as one at pos 1.
BEZEL_TEST(channel_spawn_sink_692_693,
           spawn_borrow_at_pos2_rejected_30_14) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Worker "
        "  public static method Run("
        "      owned tx: ChannelSender<int>, "
        "      owned tx2: ChannelSender<int>) "
        "    using var s := move tx; "
        "    using var s2 := move tx2; "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main("
        "      owned sender1: ChannelSender<int>, "
        "      b2: ChannelSender<int>): Task<int> "
        "    using var w := Domain.Spawn(Worker.Run, move sender1, b2); "  // b2 borrow → rejected
        "    await w; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// §30.14 / #693 residual: Domain.Spawn(entry, move X, ownedProducingCall())
// — pos 1 is a `move` of an owned local, pos 2 is an owned-returning call.
// The reopen comment on #693 observed that the pos-2 arg was falsely rejected
// with "borrow cannot be passed where owned required" when pos-1 was a `move`
// instead of another call expression. Each arg is checked independently, so
// a `move` at pos-1 must NOT affect the owned-sink acceptance of pos-2.
//
// Shape: Worker.Run(owned tx1: ChannelSender<int>, owned tx2: ChannelSender<int>)
// Domain.Spawn(Worker.Run, move sender1, ch2.NewSender())
//   pos 1: move sender1   — explicit move of an owned local (consumed)
//   pos 2: ch2.NewSender() — owned-returning call (must be accepted)
BEZEL_TEST(channel_spawn_sink_692_693,
           spawn_move_at_pos1_then_owned_call_at_pos2_accepted_30_14) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Worker "
        "  public static method Run("
        "      owned tx1: ChannelSender<int>, "
        "      owned tx2: ChannelSender<int>) "
        "    using var s1 := move tx1; "
        "    using var s2 := move tx2; "
        "    s1.Send(1); "
        "    s2.Send(2); "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main("
        "      owned sender1: ChannelSender<int>): Task<int> "
        "    using var ch2 := new Channel<int>(); "
        "    using var w := Domain.Spawn(Worker.Run, move sender1, ch2.NewSender()); "
        "    await w; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14 / #693 residual — NEGATIVE boundary: when pos-1 is a `move` and
// pos-2 is a BORROW (not an owned-producing call), pos-2 must still be
// rejected. The move at pos-1 must not suppress the owned-sink check at pos-2.
BEZEL_TEST(channel_spawn_sink_692_693,
           spawn_move_at_pos1_then_borrow_at_pos2_rejected_30_14) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Worker "
        "  public static method Run("
        "      owned tx1: ChannelSender<int>, "
        "      owned tx2: ChannelSender<int>) "
        "    using var s1 := move tx1; "
        "    using var s2 := move tx2; "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main("
        "      owned sender1: ChannelSender<int>, "
        "      b2: ChannelSender<int>): Task<int> "  // b2 is a borrow
        "    using var w := Domain.Spawn(Worker.Run, move sender1, b2); "  // b2 → rejected
        "    await w; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// ---------------------------------------------------------------------------
// #693 residual — move-of-Clone-bound-local at pos 1 (§30.14 / §28.13)
// ---------------------------------------------------------------------------
// The root cause of the residual: `var clone := sm.Clone()` was classified
// Borrowed because producerState did not recognise Shared<T>.Clone() (a
// builtin owned-returning call with no MethodDecl) as an owned producer.
// When the local entered Domain.Spawn as `move clone`, ownedArgSourceIsBorrow
// found state==Borrowed and falsely fired the owned-sink diagnostic.

// §30.14 / §28.13: Domain.Spawn(entry, move clone, ch.NewSender())
//   pos 1: move clone — clone was bound from sm.Clone() (owned Shared<T>)
//   pos 2: ch.NewSender() — owned-returning call
// Both must be accepted; this is the exact shm1.bzl repro shape.
BEZEL_TEST(channel_spawn_sink_692_693,
           spawn_move_of_clone_bound_local_pos1_then_new_sender_pos2_accepted_30_14) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Worker "
        "  public static method Run("
        "      owned sm: Shared<Mutex>, "
        "      owned tx: ChannelSender<int>) "
        "    using var s := move sm; "
        "    using var sender := move tx; "
        "    sender.Send(1); "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(); "
        "    using var sm := new Shared<Mutex>(new Mutex()); "
        "    var clone := sm.Clone(); "
        "    using var w := Domain.Spawn(Worker.Run, move clone, ch.NewSender()); "
        "    await w; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14 / §28.13: move-of-clone-bound-local at pos 1 + another clone at pos 2.
// Confirms the fix generalises: both clone-bound locals are accepted as owned
// producers when each feeds a separate owned entry parameter.
BEZEL_TEST(channel_spawn_sink_692_693,
           spawn_move_of_clone_pos1_clone_at_pos2_accepted_30_14) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Worker "
        "  public static method Run("
        "      owned sm1: Shared<Mutex>, "
        "      owned sm2: Shared<Mutex>) "
        "    using var s1 := move sm1; "
        "    using var s2 := move sm2; "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var sm := new Shared<Mutex>(new Mutex()); "
        "    var c1 := sm.Clone(); "
        "    var c2 := sm.Clone(); "
        "    using var w := Domain.Spawn(Worker.Run, move c1, move c2); "
        "    await w; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14 NEGATIVE: a borrow at pos 2 when pos 1 is move-of-clone-bound-local.
// The move at pos 1 must NOT suppress the owned-sink check at pos 2.
BEZEL_TEST(channel_spawn_sink_692_693,
           spawn_move_of_clone_pos1_borrow_at_pos2_rejected_30_14) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Worker "
        "  public static method Run("
        "      owned sm: Shared<Mutex>, "
        "      owned tx: ChannelSender<int>) "
        "    using var s := move sm; "
        "    using var sender := move tx; "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main("
        "      b: ChannelSender<int>): Task<int> "
        "    using var sm := new Shared<Mutex>(new Mutex()); "
        "    var clone := sm.Clone(); "
        "    using var w := Domain.Spawn(Worker.Run, move clone, b); "  // b borrow → rejected
        "    await w; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// §30.14 / §28.3 NEGATIVE: re-moving a moved-from clone after Spawn must be
// rejected. Confirms the move is recorded and use-after-move is diagnosed.
// Uses `var c2 := move clone` after the spawn — the `move` form always goes
// through checkRead, which catches the moved-from state (§28.3).
BEZEL_TEST(channel_spawn_sink_692_693,
           spawn_remov_of_clone_after_spawn_rejected_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Worker "
        "  public static method Run(owned sm: Shared<Mutex>) "
        "    using var s := move sm; "
        "  end Run; "
        "end Worker; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var sm := new Shared<Mutex>(new Mutex()); "
        "    var clone := sm.Clone(); "
        "    using var w := Domain.Spawn(Worker.Run, move clone); "
        "    var c2 := move clone; "  // clone moved → use-after-move → error
        "    await w; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "already moved out"));
}

