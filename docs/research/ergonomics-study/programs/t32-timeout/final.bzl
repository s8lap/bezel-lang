module Concurrency.Timeout;

import System.Threading;

public class Program
  private static async method Work(steps: int, delayMs: int, token: CancellationToken): Task<int>
    for var i in 1..steps do
      await Task.Delay(delayMs, token);
    end;
    return steps;
  end Work;

  private static async method Run(label: string, steps: int, delayMs: int): Task
    using var cts := CancellationTokenSource.WithCancelAfter(TimeSpan.FromMilliseconds(100));
    try
      var r := await Work(steps, delayMs, cts.Token);
      Console.WriteLine($"{label} result {r}");
    catch
      OperationCanceledException => Console.WriteLine("timed out")
    end;
  end Run;

  public static async method Main(): Task
    await Run("slow", 50, 20);
    await Run("fast", 2, 10);
  end Main;
end Program;
