//! case: l6_5_0013
//! doc: stdlib
//! section: 6.5
//! title: a cancelled async handler task behaves exactly as a fault
//! rule: If an asynchronous handler's task is cancelled, the behavior is the
//!   same as a fault: subsequent handlers are not invoked and
//!   OperationCanceledException propagates from the awaited RaiseAsync.
//! cites: 6.5
//! expect: run
//! output: cancelled
module Conf;

public class Pump
  private owned readonly _e: AsyncEventSource<int> := new();
  public property E: IAsyncEvent<int> => _e;

  public constructor()
  end constructor;

  public method Fire(n: int): Task
    return _e.RaiseAsync(n);
  end Fire;
end Pump;

public class Program
  public static async method Main(): Task
    using var p := new Pump();
    using var s1 := p.E.Subscribe(lambda(_) => Task.FromCancelled());
    using var s2 := p.E.Subscribe(async lambda(_) => do
      Console.WriteLine("second");
      await Task.Delay(0);
    end);
    try
      await p.Fire(1);
    catch
      OperationCanceledException => Console.WriteLine("cancelled")
    end try;
  end Main;
end Program;
