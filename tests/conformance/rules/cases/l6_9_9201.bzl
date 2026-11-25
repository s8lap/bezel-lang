//! case: l6_9_9201
//! doc: stdlib
//! section: 6.9
//! title: two producers SendAsync into a capacity-4 channel and ReceiveAll receives every item
//! rule: each item is received exactly once
//! expect: run
//! output: sum 165
//! notes: Pins #9054 (fix 3914ee801). The issue repro. 1*(1+2+3+4+5) + 10*(1+2+3+4+5) = 15 + 150 = 165
//!   (SendAsync awaits capacity; no item lost; the drain ends when both senders are disposed).
module Conf;
import System.Threading;
public static class Producers
  public static async method Produce(id: int, owned tx: ChannelSender<int>): Task
    using var sender := move tx;
    for var i in 1..5 do
      await sender.SendAsync(id * i);
    end for;
  end Produce;
end Producers;
public class Program
  public static async method Main(): Task
    using var ch := new Channel<int>(4);
    var s1 := ch.NewSender();
    var s2 := ch.NewSender();
    using var p1 := Producers.Produce(1, move s1);
    using var p2 := Producers.Produce(10, move s2);
    var sum := 0;
    await for var v in ch.ReceiveAll() do sum += v; end for;
    Console.WriteLine($"sum {sum}");
  end Main;
end Program;
