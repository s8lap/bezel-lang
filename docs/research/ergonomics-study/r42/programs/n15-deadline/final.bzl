module Net.Deadline;

import System.Threading;

public static class Lookups
  public static async method Lookup(id: int, token: CancellationToken): Task<int>
    await Task.Delay(id * 40, token);
    return id;
  end Lookup;
end Lookups;

public class Program
  public static async method Main(): Task
    using var cts := CancellationTokenSource.WithCancelAfter(TimeSpan.FromMilliseconds(140));
    using var t1 := Lookups.Lookup(1, cts.Token);
    using var t2 := Lookups.Lookup(2, cts.Token);
    using var t3 := Lookups.Lookup(3, cts.Token);
    using var t4 := Lookups.Lookup(4, cts.Token);
    using var t5 := Lookups.Lookup(5, cts.Token);
    var tasks: Array<Task<int>> := [t1, t2, t3, t4, t5];
    var finished := new Array<int>();
    var cancelled := new Array<int>();
    for var i in 0..<tasks.Count do
      try
        finished.Add(await tasks[i]);
      catch
        OperationCanceledException => cancelled.Add(i + 1)
      end;
    end;
    Console.WriteLine($"finished: {String.Join(" ", finished.Select(lambda(n) => n.ToString()))}");
    Console.WriteLine($"cancelled: {String.Join(" ", cancelled.Select(lambda(n) => n.ToString()))}");
  end Main;
end Program;
