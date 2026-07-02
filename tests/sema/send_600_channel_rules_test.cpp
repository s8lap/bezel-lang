// #600 contract — §30.15 Channel<T> type rules (instantiation + API shapes).
//
// These tests translate the Channel<T>-specific rules from §30.15 and stdlib
// §6.9 into sema diagnostics. They are EXPECTED TO FAIL against current main:
// sema today has no knowledge of Channel<T>, ChannelSender<T>, or the sendable
// derived type property. The implementation lane re-enables them from
// CMakeLists.txt.
//
// Spec derivation (§30.15 / stdlib §6.9):
//   "Channel<T> element type must be sendable; checked at instantiation per §3.6."
//   "Channel<T> and ChannelSender<T> are themselves sendable."
//   Send/Receive ownership shapes for class T.
//   TryReceive(out owned item: T?) shape.
//   ChannelClosedException on closed channel.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasNotSendableDiag(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("not sendable") != std::string::npos ||
            d.message.find("sendable") != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// §30.15: Channel<T> with non-sendable T → rejected at instantiation (§3.6).
// ---------------------------------------------------------------------------

// §30.15 / §3.6: Channel<NonSendableClass> where the class is open → rejected.
BEZEL_TEST(send_600_channel_rules,
           channel_non_sendable_class_element_rejected_30_15) {
    auto r = parseSemaAll(
        "module M; "
        "class OpenData "
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end OpenData; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<OpenData>(); "
        "  end Test; "
        "end C;");
    // Rejected: OpenData is not sendable (open class) per §30.15/§3.6.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.15 / §3.6: Channel<BorrowRec> where BorrowRec is borrow-bearing → rejected.
BEZEL_TEST(send_600_channel_rules,
           channel_borrow_bearing_record_element_rejected_30_15) {
    auto r = parseSemaAll(
        "module M; "
        "class Node end Node; "
        "record BorrowRec public Ref: Node?; end BorrowRec; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<BorrowRec>(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// ---------------------------------------------------------------------------
// §30.15 / stdlib §6.9: Send/Receive ownership shapes for class-typed T.
// (Acceptance tests — will fail now because Channel is unknown.)
// ---------------------------------------------------------------------------

// §30.15 stdlib §6.9: for class-typed T, Send takes `owned item: T` and
// Receive() returns OwningTask<T>. The pattern below must type-check.
// Channel-in-receive-domain pattern from the spec example.
BEZEL_TEST(send_600_channel_rules,
           send_receive_class_ownership_shapes_accepted_30_15) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Report "
        "  public Id: int := 0; "
        "  destructor end destructor; "
        "end Report; "
        "class Receiver "
        "  public static async method RunReceiver(owned ch: Channel<Report>): Task "
        "    using var inbox := ch; "
        "    using var item := await inbox.Receive(); "
        "  end RunReceiver; "
        "end Receiver;");
    // Accepted: OwningTask<T> receive, `using var` discharges the owned result.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.15 stdlib §6.9: for value-typed T, Send(item: T) and Receive():Task<T>.
BEZEL_TEST(send_600_channel_rules,
           send_receive_value_type_shapes_accepted_30_15) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static async method Test(): Task "
        "    using var ch := new Channel<int>(); "
        "    using var tx := ch.NewSender(); "
        "    tx.Send(42); "
        "    var n := await ch.Receive(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.15 stdlib §6.9: TryReceive(out owned item: T?) shape for class T.
// `out using var` at the call site.
BEZEL_TEST(send_600_channel_rules,
           try_receive_out_owned_shape_accepted_30_15) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Packet "
        "  public Len: int := 0; "
        "  destructor end destructor; "
        "end Packet; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Packet>(); "
        "    var got := ch.TryReceive(out using var item); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §30.14 / stdlib §6.9: ChannelSender<T> is itself sendable.
// It can appear as a spawn argument (the canonical producer pattern).
// ---------------------------------------------------------------------------

// §30.15 stdlib §6.9: ChannelSender<T> is sendable and may be passed as a
// spawn argument. The canonical multi-producer pattern.
BEZEL_TEST(send_600_channel_rules,
           channel_sender_as_spawn_arg_accepted_30_15) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Report "
        "  public Id: int := 0; "
        "  destructor end destructor; "
        "end Report; "
        "class Worker "
        "  public static method ProcessAll(owned tx: ChannelSender<Report>) "
        "    using var sender := tx; "
        "    sender.Send(new Report()); "
        "  end ProcessAll; "
        "end Worker; "
        "class Main "
        "  public static async method Go(): Task "
        "    using var inbox := new Channel<Report>(); "
        "    using var t := Domain.Spawn(Worker.ProcessAll, inbox.NewSender()); "
        "    await t; "
        "  end Go; "
        "end Main;");
    // Accepted: ChannelSender<Report> is sendable (§30.15 / stdlib §6.9).
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.15 / stdlib §6.9: ChannelSender<T> can itself be a Channel element
// (sendable object through a sendable channel). This is an advanced pattern
// used for dynamic wiring.
BEZEL_TEST(send_600_channel_rules,
           channel_sender_as_channel_element_accepted_30_15) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Report "
        "  public Id: int := 0; "
        "  destructor end destructor; "
        "end Report; "
        "class C "
        "  public static method Test() "
        // Channel<ChannelSender<Report>> is valid: ChannelSender<T> is sendable.
        "    using var ch := new Channel<ChannelSender<Report>>(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §30.14 stdlib table: stdlib types not sendable as channel elements.
// Task<T>, OwningTask<T>, Shared<T> are domain-local (§30.14 / §28.13).
// ---------------------------------------------------------------------------

// §30.14 stdlib table: Task<int> is domain-local → not sendable.
// Channel<Task<int>> must be rejected.
BEZEL_TEST(send_600_channel_rules,
           task_int_as_channel_element_rejected_30_14_stdlib) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Task<int>>(); "
        "  end Test; "
        "end C;");
    // Rejected: Task<T> is domain-local → not sendable (§30.14 stdlib table).
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.14 stdlib table: OwningTask<T> is domain-local → not sendable.
BEZEL_TEST(send_600_channel_rules,
           owned_task_as_channel_element_rejected_30_14_stdlib) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Result "
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end Result; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<OwningTask<Result>>(); "
        "  end Test; "
        "end C;");
    // Rejected: OwningTask<T> is domain-local → not sendable.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.14 / §28.13: Shared<T> is domain-local (ref-count not synchronized) →
// not sendable. Channel<Shared<SomeClass>> must be rejected.
BEZEL_TEST(send_600_channel_rules,
           shared_t_as_channel_element_rejected_28_13_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Obj "
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end Obj; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Shared<Obj>>(); "
        "  end Test; "
        "end C;");
    // Rejected: Shared<T> is domain-local → not sendable (§28.13/§30.14).
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// ---------------------------------------------------------------------------
// §30.14 stdlib table: OwningArray<T> is sendable iff element type is sendable.
// ---------------------------------------------------------------------------

// §30.14 stdlib table: OwningArray<SealedSendable> → sendable (all state owned).
BEZEL_TEST(send_600_channel_rules,
           owned_array_sendable_element_as_channel_element_accepted_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Item "
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end Item; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<OwningArray<Item>>(); "
        "  end Test; "
        "end C;");
    // Accepted: OwningArray<Item> where Item is sendable sealed class.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14 stdlib table: Array<SomeClass> carries borrows → not sendable.
BEZEL_TEST(send_600_channel_rules,
           array_class_element_as_channel_element_rejected_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "sealed class Item "
        "  public Val: int := 0; "
        "  destructor end destructor; "
        "end Item; "
        "class C "
        "  public static method Test() "
        // Array<SomeClass> carries borrow slots → not sendable.
        "    using var ch := new Channel<Array<Item>>(); "
        "  end Test; "
        "end C;");
    // Rejected: Array<T> where T is a class type carries borrows → not sendable.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotSendableDiag(r));
}

// §30.14 stdlib table: Array<string> — string is sendable-value/immutable,
// and a borrowing collection of string elements is sendable.
BEZEL_TEST(send_600_channel_rules,
           array_string_as_channel_element_accepted_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Array<string>>(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.14 stdlib table: Array<int> — primitives are sendable values;
// a borrowing collection of sendable-value elements is sendable.
BEZEL_TEST(send_600_channel_rules,
           array_int_as_channel_element_accepted_30_14) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Test() "
        "    using var ch := new Channel<Array<int>>(); "
        "  end Test; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
