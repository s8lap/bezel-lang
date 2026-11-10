module Concurrency.Pipeline;

import System.Threading;

public class Program
  // Each stage owns its sender: disposing the last sender closes the channel (end-of-stream).
  private static async method Produce(owned sink: ChannelSender<string>): Task
    for var w in "alpha beta gamma".Split(' ') do
      await sink.SendAsync(w);
    end;
  end Produce;

  private static async method Upper(input: Channel<string>, owned output: ChannelSender<string>): Task
    await for var w in input.ReceiveAll() do
      await output.SendAsync(w.ToUpper());
    end;
  end Upper;

  public static async method Main(): Task
    using var words := new Channel<string>();
    using var upper := new Channel<string>();
    // v2: TaskScope.Spawn closures cannot take the senders (a captured owned binding cannot be
    // moved inside a closure, §28.3). Start the stages as plain task-returning calls instead.
    using var p := Produce(words.NewSender());
    using var u := Upper(words, upper.NewSender());
    var n := 0;
    await for var w in upper.ReceiveAll() do
      Console.WriteLine(w);
      n++;
    end;
    await p;
    await u;
    Console.WriteLine($"done {n}");
  end Main;
end Program;
