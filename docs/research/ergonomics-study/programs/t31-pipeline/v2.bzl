module Concurrency.Pipeline;

import System.Threading;

public class Program
  private static async method Produce(sink: ChannelSender<string>): Task
    for var w in "alpha beta gamma".Split(' ') do
      await sink.SendAsync(w);
    end;
  end Produce;

  private static async method Upper(input: Channel<string>, output: ChannelSender<string>): Task
    await for var w in input.ReceiveAll() do
      await output.SendAsync(w.ToUpper());
    end;
  end Upper;

  public static async method Main(): Task
    using var words := new Channel<string>();
    using var upper := new Channel<string>();
    await using var scope := new TaskScope();
    var s1 := words.NewSender();
    var s2 := upper.NewSender();
    scope.Spawn(async lambda() => do
      await Produce(move s1);
    end);
    scope.Spawn(async lambda() => do
      await Upper(words, move s2);
    end);
    var n := 0;
    await for var w in upper.ReceiveAll() do
      Console.WriteLine(w);
      n++;
    end;
    await scope.Join();
    Console.WriteLine($"done {n}");
  end Main;
end Program;
