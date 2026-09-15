//! case: l6_5_0003
//! doc: stdlib
//! section: 6.5
//! title: a synchronous handler exception stops later handlers — first error wins
//! rule: If a synchronous handler throws, subsequent handlers are not
//!   invoked and the exception propagates out of Raise to the raiser.
//! cites: 6.5
//! expect: run
//! stdout: caught
module Conf;

public class Ticker
  private owned readonly _tick: EventSource<int> := new();
  public property Tick: IEvent<int> => _tick;

  public constructor()
  end constructor;

  public method Fire(n: int)
    _tick.Raise(n);
  end Fire;
end Ticker;

public class Program
  public static method Main()
    using var t := new Ticker();
    using var s1 := t.Tick.Subscribe(lambda(n) => throw new InvalidOperationException("boom"));
    using var s2 := t.Tick.Subscribe(lambda(n) => Console.WriteLine("second"));
    try
      t.Fire(1);
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
