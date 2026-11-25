// Codegen / runtime — stdlib §6.9 v0.359 (#9054): a bounded channel fed by
// `SendAsync` and drained by `ReceiveAll()` loses items, and at capacity 1
// hangs.
//
// THE DEFECT (src/runtime/bezel_runtime_concurrency.cpp,
// channel_send_async_core): when the consumer's `await for ... in
// ch.ReceiveAll()` finds the channel empty and open, its MoveNextAsync parks
// a drain-waiter task on the channel (`recv_all_waiter_task`). The sync
// `Send` (channel_send_core) hands a new item straight to that waiter, but
// `SendAsync` had no such arm: it buffered the item and left the waiter
// parked. The waiter then woke only at close, with the "drained" status, so
// the enumeration ended with the buffered items never yielded (capacity 4:
// sum 25 instead of 165, every run); and once the buffer filled, every
// further SendAsync parked waiting for a Receive that never came, the last
// sender was never disposed, and the program hung (capacity 1).
//
// THE CONTRACT (stdlib §6.9, v0.359):
//   "`SendAsync` **awaits capacity**: the returned task completes when the
//    item has been accepted (immediately when space is free, and always
//    immediately on an unbounded channel). ... Ordering and exactly-once
//    delivery are unchanged."
//   ReceiveAll "yields each item with `Receive`'s exact transfer semantics
//    ..., suspends while empty-and-open, and completes the enumeration
//    **normally** on drained-and-closed".
//   "Disposing the last sender closes the channel; buffered items remain
//    receivable".
// So every item sent is yielded exactly once before the enumeration ends.
//
// The capacity loop ranges over an index (`Cap(ci)`) rather than an array
// literal: a `for` over an inline literal loses its position across an
// `await` in the body, which is #6185, a separate open defect.
//
// `guard` keeps the channel open while the producers start: a producer that
// never suspends (capacity 16) finishes and disposes its sender at once, and
// were it the only live sender that would close the channel by design
// ("Disposing the last sender closes the channel").
//
// DETERMINISM: the loss depends on whether the consumer parks before a
// producer sends, so each row runs many rounds inside ONE program and
// prints a count; a single lost item or a hang fails the row. A hang is
// caught by the harness's run deadline (a timeout fails the test).
//
// Each `Main` first suspends on `await Task.Delay(1)`, so every round runs as
// a main-domain continuation. Without it, round 1 runs on the startup thread
// in parallel with the producer continuations it wakes (#9195, a separate
// open runtime defect), and the class row's non-atomic static counters can
// lose an update.
//
// ORACLES: runAot / runAotChecked only. No `wasSkipped` absorbing guard:
// `r.ran` is asserted directly.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

// =====================================================================
// stdlib §6.9 v0.359: 1, 2 and 3 producers x capacities 1, 2, 4, 16, 20
// rounds each (240 rounds). Each producer `SendAsync`s five items; the
// consumer drains with ReceiveAll. Every round must receive the full sum.
// Fail-without: the program hangs at the first capacity-1 round (run
// deadline); with the capacity-1 rounds removed it reports lost items.
// =====================================================================
BEZEL_TEST(channel_send_async_receive_all_9054,
           every_sent_item_is_yielded_across_producers_and_capacities_stdlib_6_9) {
    JitResult r = runAot(R"BZ(
module P;
import System.Threading;
public static class Producers
  public static async method Produce(id: int, owned tx: ChannelSender<int>): Task
    using var sender := move tx;
    for var i in 1..5 do
      await sender.SendAsync(id * i);
    end;
  end Produce;
  public static async method Round(cap: int, producers: int): Task<int>
    using var ch := new Channel<int>(cap);
    using var tasks := new OwningArray<Task>();
    var guard := ch.NewSender();
    var id := 1;
    for var p in 1..producers do
      var s := ch.NewSender();
      tasks.Add(Produce(id, move s));
      id := id * 10;
    end;
    guard.Dispose();
    var sum := 0;
    await for var v in ch.ReceiveAll() do sum += v; end;
    return sum;
  end Round;
  public static method Cap(ci: int): int
    if ci = 1 then return 1; end;
    if ci = 2 then return 2; end;
    if ci = 3 then return 4; end;
    return 16;
  end Cap;
end Producers;
public class Program
  public static async method Main(): Task
    await Task.Delay(1);  // leave the startup prefix first (#9195)
    var good := 0;
    var bad := 0;
    for var producers in 1..3 do
      var expected := 0;
      var id := 1;
      for var p in 1..producers do expected += 15 * id; id := id * 10; end;
      for var ci in 1..4 do
        var cap := Producers.Cap(ci);
        for var round in 1..20 do
          var got := await Producers.Round(cap, producers);
          if got = expected then good += 1; else bad += 1; end;
        end;
      end;
    end;
    Console.WriteLine($"good {good} bad {bad}");
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("good 240 bad 0\n"));
}

// =====================================================================
// stdlib §6.9 v0.359, class-typed items under the checked oracle: two
// producers `SendAsync` five owned `Item`s each into a `Channel<Item>(4)`,
// 200 rounds. Every item must be YIELDED (received count 10 per round), and
// every item must be disposed exactly once (net live count 0, cumulative
// destructor count 2000), with no leak and no double free under ASan/LSan.
// Fail-without: the drain ends early, so the received count falls short
// (the unyielded items are disposed by the channel's own disposal, so a
// leak oracle alone would not see the loss — the received count does).
// =====================================================================
BEZEL_TEST(channel_send_async_receive_all_9054,
           class_items_are_all_yielded_and_disposed_once_stdlib_6_9) {
    JitResult r = runAotChecked(R"BZ(
module P;
import System.Threading;
public sealed class Item
  public V: int;
  public static live: int := 0;
  public static disposed: int := 0;
  public constructor(v: int) Item.live := Item.live + 1; V := v; end constructor;
  destructor Item.live := Item.live - 1; Item.disposed := Item.disposed + 1; end destructor;
end Item;
public static class Producers
  public static async method Produce(id: int, owned tx: ChannelSender<Item>): Task
    using var sender := move tx;
    for var i in 1..5 do
      await sender.SendAsync(new Item(id * i));
    end;
  end Produce;
  public static async method Round(): Task<int>
    using var ch := new Channel<Item>(4);
    var s1 := ch.NewSender();
    var s2 := ch.NewSender();
    using var p1 := Produce(1, move s1);
    using var p2 := Produce(10, move s2);
    var received := 0;
    await for var it in ch.ReceiveAll() do
      using var held := move it;
      received += 1;
    end;
    return received;
  end Round;
end Producers;
public class Program
  public static async method Main(): Task
    await Task.Delay(1);  // leave the startup prefix first (#9195)
    var good := 0;
    for var round in 1..200 do
      if await Producers.Round() = 10 then good += 1; end;
    end;
    Console.WriteLine($"good {good} live {Item.live} disposed {Item.disposed}");
  end Main;
end Program;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("good 200 live 0 disposed 2000\n"));
}
