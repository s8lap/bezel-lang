//! case: l6_5_0019
//! doc: stdlib
//! section: 6.5
//! title: a handler capturing this is admitted when the Subscription is an owned field
//! rule: Storing a Subscription in an owned field of `this` resolves the
//!   bounding object to `this` (spec §28.14 form 3), so a handler capturing
//!   `this` (or state reached through it) is admitted regardless of how the
//!   publisher was reached.
//! cites: 6.5
//! expect: run
//! stdout: 3
module Conf;

public class Logger
  public constructor()
  end constructor;

  public method Write(n: int)
    Console.WriteLine(n);
  end Write;
end Logger;

public class TickObserver
  private owned _sub: Subscription;
  private _log: Logger;

  public constructor(ticks: IEvent<int>, logger: Logger)
    _log := logger;
    _sub := ticks.Subscribe(lambda(n) => _log.Write(n));
  end constructor;
end TickObserver;

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
    using var log := new Logger();
    using var obs := new TickObserver(t.Tick, log);
    t.Fire(3);
  end Main;
end Program;
