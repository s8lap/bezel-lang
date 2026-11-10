module Concurrency.FanOut;

import System.Diagnostics;

public class Program
  private static async method Fetch(delayMs: int, value: int): Task<int>
    await Task.Delay(delayMs);
    return value;
  end Fetch;

  public static async method Main(): Task
    using var sw := Stopwatch.StartNew();
    // Fresh tasks may not sit in a borrowing literal (§15.4/§30.5): name each one first.
    using var a := Fetch(30, 1);
    using var b := Fetch(10, 2);
    using var c := Fetch(20, 3);
    using var all := Task.WhenAll([a, b, c]);
    var results := await all;
    sw.Stop();
    Console.WriteLine($"results {String.Join(" ", results.Select(lambda(r) => r.ToString()))}");
    Console.WriteLine($"sum {results.Sum()}");
    Console.WriteLine($"concurrent: {sw.ElapsedMilliseconds < 55}");
  end Main;
end Program;
