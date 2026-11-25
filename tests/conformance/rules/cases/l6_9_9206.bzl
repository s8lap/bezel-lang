//! case: l6_9_9206
//! doc: stdlib
//! section: 6.9
//! title: class-typed items through a bounded channel are each received exactly once
//! rule: each item is received exactly once
//! expect: run
//! output: sum 165 live 0
//! notes: Pins #9054 (fix 3914ee801). Class (sealed Job) items: 1*(1..5) + 10*(1..5) = 165. Each received Job
//!   is disposed by the consumer, so the live counter ends 0.
module Conf;
import System.Threading;
public sealed class Job
  public Id: int;
  public constructor(id: int)
    Id := id;
    Counter.Live := Counter.Live + 1;
  end constructor;
  destructor
    Counter.Live := Counter.Live - 1;
  end destructor;
end Job;
public static class Counter
  public static Live: int := 0;
end Counter;
public static class Producers
  public static async method Produce(scale: int, owned tx: ChannelSender<Job>): Task
    using var sender := move tx;
    for var i in 1..5 do
      await sender.SendAsync(new Job(scale * i));
    end for;
  end Produce;
end Producers;
public class Program
  public static async method Main(): Task
    using var ch := new Channel<Job>(2);
    var s1 := ch.NewSender();
    var s2 := ch.NewSender();
    using var p1 := Producers.Produce(1, move s1);
    using var p2 := Producers.Produce(10, move s2);
    var sum := 0;
    await for var j in ch.ReceiveAll() do
      sum += j.Id;
      j.Dispose();
    end for;
    Console.WriteLine($"sum {sum} live {Counter.Live}");
  end Main;
end Program;
