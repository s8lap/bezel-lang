module Net.Limit;

import System.Threading;

public class Tracker
  private _inside: int := 0;
  private _max: int := 0;
  private _done: int := 0;
  public property Max: int => _max;
  public property Done: int => _done;
  public method Enter()
    _inside++;
    if _inside > _max then _max := _inside; end;
  end Enter;
  public method Leave()
    _inside--;
    _done++;
  end Leave;
end Tracker;

public static class Jobs
  public static async method Run(gate: Semaphore, tracker: Tracker): Task
    using var permit := await gate.AcquireAsync();
    tracker.Enter();
    await Task.Delay(20);
    tracker.Leave();
  end Run;
end Jobs;

public class Program
  public static async method Main(): Task
    using var gate := new Semaphore(2);
    using var tracker := new Tracker();
    using var jobs := new OwningArray<Task>();
    for var i in 1..6 do
      jobs.Add(Jobs.Run(gate, tracker));
    end;
    for var j in jobs do
      await j;
    end;
    Console.WriteLine($"max concurrent {tracker.Max}");
    Console.WriteLine($"completed {tracker.Done}");
  end Main;
end Program;
