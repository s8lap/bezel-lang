module Pipe.FanIn;

import System.Threading;

public static class Producers
  public static async method Produce(id: int, owned tx: ChannelSender<int>): Task
    using var sender := move tx;
    for var i in 1..5 do
      await sender.SendAsync(i * id);
    end;
  end Produce;
end Producers;

public class Program
  public static async method Main(): Task
    using var ch := new Channel<int>(4);
    using var p1 := Producers.Produce(1, ch.NewSender());
    using var p2 := Producers.Produce(10, ch.NewSender());
    using var p3 := Producers.Produce(100, ch.NewSender());
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
