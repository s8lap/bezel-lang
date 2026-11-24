module Tools.Jobs;

import System.Threading;

// r42: `IFailure` is an unresolved type name (#8414)
public class TransientError(message: string): Exception(message)
end TransientError;

public static class Jobs
  public static async method Ok(token: CancellationToken): Task<int>
    await Task.Delay(5, token);
    return 1;
  end Ok;

  public static async method Flaky(token: CancellationToken): Task<int>
    await Task.Delay(5, token);
    throw new TransientError("upstream 503");
  end Flaky;

  public static async method Buggy(token: CancellationToken): Task<int>
    await Task.Delay(5, token);
    throw new InvalidOperationException("bad state");
  end Buggy;

  public static async method Slow(token: CancellationToken): Task<int>
    await Task.Delay(5000, token);
    return 4;
  end Slow;
end Jobs;

public class Runner
  // the boundary: a job runner that must keep going
  public static async method RunAll(token: CancellationToken): Task
    var names: Array<string> := ["ok", "flaky", "buggy", "slow"];
    for var name in names do
      try
        var r := await Run(name, token);
        Console.WriteLine($"{name}: {r}");
      catch
        OperationCanceledException => throw,
        var f: TransientError =>   // r42: no IFailure arm (#8414)
           Console.WriteLine($"{name}: transient: {f.Message}"),
        var ex: Exception => Console.WriteLine($"{name}: BUG {ex}")
      end try;
    end;
  end RunAll;

  private static async method Run(name: string, token: CancellationToken): Task<int>
    return (case name of
      "ok" => await Jobs.Ok(token),
      "flaky" => await Jobs.Flaky(token),
      "buggy" => await Jobs.Buggy(token),
      _ => await Jobs.Slow(token));
  end Run;
end Runner;

public class Program
  public static async method Main(): Task
    using var cts := CancellationTokenSource.WithCancelAfter(TimeSpan.FromMilliseconds(200));
    try
      await Runner.RunAll(cts.Token);
    catch
      OperationCanceledException => Console.WriteLine("runner stopped: cancelled")
    end try;
  end Main;
end Program;
