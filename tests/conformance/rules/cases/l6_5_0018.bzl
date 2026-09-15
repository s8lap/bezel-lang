//! case: l6_5_0018
//! doc: stdlib
//! section: 6.5
//! title: a handler capturing a constructor parameter other than this is rejected
//! rule: Captured constructor parameters other than `this` are not admitted —
//!   a constructor parameter is a borrow of a caller local and does not
//!   provably outlive the constructed object.
//! cites: 6.5
//! expect: reject
//! diagnostic: capture
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
    _sub := ticks.Subscribe(lambda(n) => logger.Write(n));
  end constructor;
end TickObserver;
