module Net.Fetch;

import System.Threading;

// r42: `IFailure` is an unresolved type name (#8414)
public class UpstreamError(message: string): Exception(message)
end UpstreamError;

public static class Fetcher
  public static async method Fetch(name: string, ms: int, fail: bool, token: CancellationToken): Task<string>
    await Task.Delay(ms, token);
    if fail then
      throw new UpstreamError($"upstream {name}");
    end;
    return name.ToUpper();
  end Fetch;

  public static async method FetchAll(failC: bool): Task
    var names: Array<string> := ["a", "b", "c"];
    var delays: Array<int> := [20, 40, 60];
    var results := new Array<string>(3, "");
    try
      await using var scope := new TaskScope();
      scope.CancelAfter(TimeSpan.FromMilliseconds(300));
      for var i in 0..<3 do
        var idx := i;
        scope.Spawn(async lambda() => do
          results[idx] := await Fetch(names[idx], delays[idx], failC and idx = 2, scope.Token);
        end);
      end;
      await scope.Join();
    catch
      var f: UpstreamError => Console.WriteLine($"failed: {f.Message}")
    end try;
    Console.WriteLine($"done: {String.Join(" ", results.Where(lambda(r) => r.Length > 0))}");
  end FetchAll;
end Fetcher;

public class Program
  public static async method Main(): Task
    await Fetcher.FetchAll(true);
    await Fetcher.FetchAll(false);
  end Main;
end Program;
