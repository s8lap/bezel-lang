//! case: l6_9_9205
//! doc: stdlib
//! section: 6.9
//! title: SendAsync and Send producers on one unbounded channel both reach the ReceiveAll drain
//! rule: each item is received exactly once
//! expect: run
//! output: good 50 bad 0
//! notes: Pins #9054 (fix 3914ee801). ProduceA sends 10 x 1 and ProduceS sends 10 x 100: every round sums 10 +
//!   1000 = 1010 (50 rounds).
module Conf;
import System.Threading;
public static class Producers
  public static async method ProduceA(owned tx: ChannelSender<int>): Task
    using var sender := move tx;
    for var i in 1..10 do await sender.SendAsync(1); end for;
  end ProduceA;
  public static async method ProduceS(owned tx: ChannelSender<int>): Task
    using var sender := move tx;
    for var i in 1..10 do sender.Send(100); await Task.Delay(1); end for;
  end ProduceS;
  public static async method Round(): Task<int>
    using var ch := new Channel<int>();
    var s1 := ch.NewSender();
    var s2 := ch.NewSender();
    using var p1 := ProduceA(move s1);
    using var p2 := ProduceS(move s2);
    var sum := 0;
    await for var v in ch.ReceiveAll() do sum += v; end for;
    return sum;
  end Round;
end Producers;
public class Program
  public static async method Main(): Task
    var good := 0;
    var bad := 0;
    for var round in 1..50 do
      if await Producers.Round() = 1010 then good += 1; else bad += 1; end if;
    end for;
    Console.WriteLine($"good {good} bad {bad}");
  end Main;
end Program;
