//! case: l6_5_0006
//! doc: stdlib
//! section: 6.5
//! title: Raise invokes handlers in subscription order
//! rule: Raise and RaiseAsync invoke handlers in subscription order, over a
//!   snapshot of the subscriber list taken when the raise begins.
//! cites: 6.5
//! expect: run
//! output: first
//!   second
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
    using var s1 := t.Tick.Subscribe(lambda(_) => Console.WriteLine("first"));
    using var s2 := t.Tick.Subscribe(lambda(_) => Console.WriteLine("second"));
    t.Fire(1);
  end Main;
end Program;
