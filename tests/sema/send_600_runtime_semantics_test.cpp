// #600 contract — §30.14/§30.15 runtime-semantics tests.
//
// These tests cover RUNTIME BEHAVIOR: channel round-trips, domain join,
// DomainFaultedException propagation, and channel-close drain semantics.
// They require both the sema layer AND a runtime Domain/Channel implementation.
// They are EXPECTED TO FAIL against current main on two counts:
//   (1) sema doesn't know Channel/Domain (the same unimplemented features), AND
//   (2) no runtime Domain.Spawn / Channel engine exists.
//
// Clearly segregated here from the purely-sema tests so the impl lane can
// track them separately. Re-enabled from CMakeLists.txt after both sema AND
// runtime phases land.
//
// Spec derivation:
//   §30.15: "Send consumes the value in the sending domain; Receive produces
//     it owned in the receiving domain."
//   §30.15: "Disposing the last ChannelSender<T> closes the channel: items
//     already sent remain receivable (the channel drains), and after the drain,
//     receives raise ChannelClosedException."
//   §30.14: "An exception that propagates out of the entry ... raises
//     DomainFaultedException ... whose Message and FaultStackTrace carry copies
//     of the original exception's message and stack-trace text."
//   §30.14: "Awaiting an OwningTask<R> join follows Pattern B: ownership of
//     the result transfers to the awaiter."
//
// These are JIT-executed end-to-end tests (like tests/codegen/*) using the
// codegen_support.hpp harness, not pure-sema tests. They live in sema/ for
// organizational convenience (issue #600 scope) but use the JIT path.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ---------------------------------------------------------------------------
// §30.15: Single channel send/receive round-trip with ownership transfer.
// Sender sends one owned Payload; receiver awaits and obtains ownership.
// ---------------------------------------------------------------------------

// §30.15 spec example (adapted): unbounded channel round-trip.
// The spawned domain sends one item; the main domain receives it.
// Per §30.15: Send transfers ownership into the channel; await Receive()
// (OwningTask<T> Pattern B) transfers ownership to the receiver.
BEZEL_TEST(send_600_runtime,
           single_send_receive_round_trip_30_15) {
    auto r = runAot(
        "module M; "
        "sealed class Payload "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "  destructor end destructor; "
        "end Payload; "
        "class Producer "
        "  public static method SendOne(owned tx: ChannelSender<Payload>) "
        "    using var sender := move tx; "
        "    sender.Send(new Payload(99)); "
        "  end SendOne; "
        "end Producer; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var inbox := new Channel<Payload>(); "
        "    using var t := Domain.Spawn(Producer.SendOne, inbox.NewSender()); "
        "    await t; "
        "    using var item := await inbox.Receive(); "
        "    return item.Val; "
        "  end Main; "
        "end Program;");
    // §30.15: ownership of the Payload transfers to the awaiter.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// ---------------------------------------------------------------------------
// §30.15: Last-sender dispose closes; drain then ChannelClosedException.
// ---------------------------------------------------------------------------

// §30.15: "Disposing the last ChannelSender<T> closes the channel: items
// already sent remain receivable (the channel drains), and after the drain,
// receives raise ChannelClosedException."
BEZEL_TEST(send_600_runtime,
           last_sender_dispose_closes_drain_then_exception_30_15) {
    auto r = runAot(
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
        "    tx.Send(new Item(1)); "
        "    tx.Send(new Item(2)); "
        // Dispose the sender: closes the channel; buffered items remain.
        "    tx.Dispose(); "
        "    using var a := await ch.Receive(); "   // drains item 1
        "    using var b := await ch.Receive(); "   // drains item 2
        // Channel is now drained+closed; next Receive must raise
        // ChannelClosedException.  Per §31.2 / §A.6 Bezel catch syntax:
        //   `catch TypeName => statement;`  (no parentheses).
        "    try "
        "      using var c := await ch.Receive(); "
        "      return -1; "   // must not reach
        "    catch ChannelClosedException => return a.Val + b.Val; "   // 1 + 2 = 3
        "    end; "
        "  end Main; "
        "end Program;");
    // §30.15 drain semantics: items received in order; closed raises exception.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ---------------------------------------------------------------------------
// §30.14: Domain.Spawn join returns result (OwningTask<R> Pattern B).
// ---------------------------------------------------------------------------

// §30.14: "Awaiting an OwningTask<R> join follows Pattern B: ownership of the
// sendable result transfers to the awaiter."
BEZEL_TEST(send_600_runtime,
           domain_spawn_join_returns_result_30_14) {
    auto r = runAot(
        "module M; "
        "sealed class Computed "
        "  public Val: int := 0; "
        "  public constructor(v: int) Val := v; end constructor; "
        "  destructor end destructor; "
        "end Computed; "
        "class Worker "
        "  public static method Compute(): owned Computed "
        "    return new Computed(7); "
        "  end Compute; "
        "end Worker; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(Worker.Compute); "
        "    using var res := await t; "   // Pattern B: ownership transfers
        "    return res.Val; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------------
// §30.14: Entry fault surfaces as DomainFaultedException with copied Message.
// ---------------------------------------------------------------------------

// §30.14: "An exception that propagates out of the entry does not cross the
// boundary as an object ... The join instead raises DomainFaultedException ...
// whose Message and FaultStackTrace carry copies of the original exception's
// message and stack-trace text."
BEZEL_TEST(send_600_runtime,
           entry_fault_surfaces_as_domain_faulted_exception_30_14) {
    // The spawned domain throws; the join must raise DomainFaultedException
    // (not the original exception class), and Message must be non-empty.
    auto r = runAot(
        "module M; "
        "class Worker "
        "  public static method Fail() "
        "    throw new Exception(\"domain-fault-test\"); "
        "  end Fail; "
        "end Worker; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(Worker.Fail); "
        "    try "
        "      await t; "
        "      return -1; "   // must not reach
        // §31.2 Bezel catch-with-binding: `catch var e: Type => statement;`
        // §A.7 Bezel conditional: `value if cond else other` (not C `cond ? a : b`).
        "    catch var e: DomainFaultedException => "
        // Verify Message is non-empty (a copy of the original text).
        "      return (if e.Message.Length > 0 then 1 else -2); "
        "    end; "
        "  end Main; "
        "end Program;");
    // §30.14: DomainFaultedException caught with non-empty Message.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ---------------------------------------------------------------------------
// §30.14 / fix #1228: Domain.Spawn with a non-capturing lambda entry.
//
// The spec (§30.14) lists non-capturing lambdas as valid spawn entries:
//   Domain.Spawn(entry: () -> ()): owned Task
// A codegen bug caused SIGSEGV at runtime: the lambda closure cell was not
// passed as the adapter arg, so the spawned domain's call dereferenced a null
// closure. Fix: build a spawn adapter that loads fn+env from the closure cell
// and calls fn(env); release the closure in the spawned domain.
// ---------------------------------------------------------------------------

// §30.14 / #1228: expression-body non-capturing lambda spawn runs without crash.
// The lambda calls Console.WriteLine — a void-returning spawn-entry pattern.
BEZEL_TEST(send_600_runtime,
           spawn_noncapturing_lambda_expr_body_runs_1228) {
    // spawn entry is a non-capturing expression-body lambda.
    // The spawned domain must complete without SIGSEGV/SIGABRT (exit 139/134).
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(lambda() => Console.WriteLine(\"hi\")); "
        "    await t; "
        "    return 42; "
        "  end Main; "
        "end Program;");
    // §30.14: entry ran in spawned domain; join completed; exit code = 42.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §30.14 / #1228: block-body non-capturing lambda spawn runs without crash.
// The block body performs a side-effect; the spawning domain returns a value.
BEZEL_TEST(send_600_runtime,
           spawn_noncapturing_lambda_block_body_runs_1228) {
    // spawn entry is a non-capturing block-body lambda.
    auto r = runAot(
        "module M; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(lambda() => do "
        "      Console.WriteLine(\"block\"); "
        "    end); "
        "    await t; "
        "    return 7; "
        "  end Main; "
        "end Program;");
    // §30.14: block-body lambda entry ran; join completed; exit code = 7.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
