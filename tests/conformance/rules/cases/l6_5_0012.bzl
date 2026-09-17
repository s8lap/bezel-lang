//! case: l6_5_0012
//! doc: stdlib
//! section: 6.5
//! title: a faulted async handler stops later handlers and propagates from RaiseAsync
//! rule: If an asynchronous handler's task faults, subsequent handlers are not
//!   invoked and the exception propagates from the awaited RaiseAsync.
//! cites: 6.5
//! expect: run
//! output: caught
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
      await Task.Delay(0);
      throw new InvalidOperationException("boom");
    end);
    using var s2 := p.E.Subscribe(async lambda(_) => do
      Console.WriteLine("second");
      await Task.Delay(0);
    end);
    try
      await p.Fire(1);
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end try;
  end Main;
end Program;
