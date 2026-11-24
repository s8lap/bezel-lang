module Pipe.FanIn;

import System.Threading;

public static class Producers
  public static async method Produce(id: int, owned tx: ChannelSender<int>): Task
    using var sender := move tx;
    for var i in 1..5 do
      await Task.Delay(1);
      sender.Send(i * id);   // r42: SendAsync on a bounded channel loses items and hangs (new)
    end;
  end Produce;
end Producers;

public class Program
  public static async method Main(): Task
    using var ch := new Channel<int>();   // r42: unbounded, since bounded + SendAsync hangs (new)
    // mint every sender before starting any producer: a producer that finishes synchronously
    // disposes the then-last sender and closes the channel (stdlib §6.9)
    var s1 := ch.NewSender();
    var s2 := ch.NewSender();
    var s3 := ch.NewSender();
    using var p1 := Producers.Produce(1, move s1);
    using var p2 := Producers.Produce(10, move s2);
    using var p3 := Producers.Produce(100, move s3);
    var count := 0;
    var sum := 0;
    await for var v in ch.ReceiveAll() do
      count++;
      sum += v;
    end;
    var all: Array<Task> := [p1, p2, p3];
    await Task.WhenAll(all);
    Console.WriteLine($"received {count} items, sum {sum}");
  end Main;
end Program;
