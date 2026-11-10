module Concurrency.FanOut;

import System.Diagnostics;

public class Program
  private static async method Fetch(delayMs: int, value: int): Task<int>
    await Task.Delay(delayMs);
    return value;
  end Fetch;

  public static async method Main(): Task
    var sw := Stopwatch.StartNew();
    var results := await Task.WhenAll([Fetch(30, 1), Fetch(10, 2), Fetch(20, 3)]);
    sw.Stop();
    Console.WriteLine($"results {String.Join(" ", results.Select(lambda(r) => r.ToString()))}");
    Console.WriteLine($"sum {results.Sum()}");
    Console.WriteLine($"concurrent: {sw.ElapsedMilliseconds < 55}");
  end Main;
end Program;
