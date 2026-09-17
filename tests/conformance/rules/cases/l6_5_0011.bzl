//! case: l6_5_0011
//! doc: stdlib
//! section: 6.5
//! title: RaiseAsync awaits each handler's task before invoking the next
//! rule: RaiseAsync returns a Task that completes when every handler's task
//!   completes; each handler's task is awaited before the next is invoked.
//! cites: 6.5
//! expect: run
//! output: a1
//!   a2
//!   b1
//!   b2
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
    using var s1 := p.E.Subscribe(async lambda(_) => do
      Console.WriteLine("a1");
      await Task.Delay(0);
      Console.WriteLine("a2");
    end);
    using var s2 := p.E.Subscribe(async lambda(_) => do
      Console.WriteLine("b1");
      await Task.Delay(0);
      Console.WriteLine("b2");
    end);
    await p.Fire(1);
  end Main;
end Program;
