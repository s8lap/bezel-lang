//! case: l6_5_0001
//! doc: stdlib
//! section: 6.5
//! title: Subscribe then Raise invokes the handler with the payload
//! rule: Subscribe registers a handler and returns an owned
//!   Subscription; Raise invokes handlers in subscription order.
//! cites: 6.5
//! expect: run
//! stdout: 42
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
    using var sub := t.Tick.Subscribe(lambda(n) => Console.WriteLine(n));
    t.Fire(42);
  end Main;
end Program;
