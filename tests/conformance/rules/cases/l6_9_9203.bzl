//! case: l6_9_9203
//! doc: stdlib
//! section: 6.9
//! title: Receive then ReceiveAll on a bounded channel delivers each item once over 100 rounds
//! rule: each item is received exactly once
//! expect: run
//! output: good 100 bad 0
//! notes: Pins #9054 (fix 3914ee801). Each round sums 1*(1..6) + 10*(1..6) = 21 + 210 = 231 across 4 single
//!   Receives and a ReceiveAll drain, capacity 1..5. Every round is 231: good 100.
module Conf;
import System.Threading;
public static class Producers
  public static async method Produce(id: int, owned tx: ChannelSender<int>): Task
    using var sender := move tx;
    for var i in 1..6 do
      await sender.SendAsync(id * i);
    end for;
  end Produce;
  public static async method Round(cap: int): Task<int>
    using var ch := new Channel<int>(cap);
    var s1 := ch.NewSender();
    var s2 := ch.NewSender();
    using var p1 := Produce(1, move s1);
    using var p2 := Produce(10, move s2);
    var sum := 0;
    for var k in 1..4 do
      sum += await ch.Receive();
    end for;
    await for var v in ch.ReceiveAll() do sum += v; end for;
    return sum;
  end Round;
end Producers;
public class Program
  public static async method Main(): Task
    var good := 0;
    var bad := 0;
    for var round in 1..100 do
      var got := await Producers.Round(1 + round mod 5);
      if got = 231 then good += 1; else bad += 1; end if;
    end for;
    Console.WriteLine($"good {good} bad {bad}");
  end Main;
end Program;
