//! case: l6_9_9202
//! doc: stdlib
//! section: 6.9
//! title: a capacity-1 channel with two awaiting producers delivers every item to ReceiveAll
//! rule: each item is received exactly once
//! expect: run
//! output: sum 66
//! notes: Pins #9054 (fix 3914ee801). Smallest capacity maximises parked senders: 1*(1+2+3) + 10*(1+2+3) = 6 +
//!   60 = 66.
module Conf;
import System.Threading;
public static class Producers
  public static async method Produce(id: int, owned tx: ChannelSender<int>): Task
    using var sender := move tx;
    for var i in 1..3 do
      await sender.SendAsync(id * i);
    end for;
  end Produce;
end Producers;
public class Program
  public static async method Main(): Task
    using var ch := new Channel<int>(1);
    var s1 := ch.NewSender();
    var s2 := ch.NewSender();
    using var p1 := Producers.Produce(1, move s1);
    using var p2 := Producers.Produce(10, move s2);
    var sum := 0;
    await for var v in ch.ReceiveAll() do sum += v; end for;
    Console.WriteLine($"sum {sum}");
  end Main;
end Program;
