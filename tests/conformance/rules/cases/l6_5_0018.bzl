//! case: l6_5_0018
//! doc: stdlib
//! section: 6.5
//! title: a handler capturing a constructor parameter other than this is rejected
//! rule: A captured constructor parameter other than `this` is not admitted
//!   where it contributes a borrow source (spec §28.3) — a class-, interface-
//!   or plain function-typed parameter, or an `owned` one: such a parameter is
//!   a borrow of a caller local and does not provably outlive the constructed
//!   object (v0.301).
//! cites: 6.5, 28.3, 28.14
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
