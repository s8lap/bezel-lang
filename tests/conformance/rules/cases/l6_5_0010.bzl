//! case: l6_5_0010
//! doc: stdlib
//! section: 6.5
//! title: EventSource of unit provides a no-argument Raise overload
//! rule: No-payload events use `EventSource<()>`; both `EventSource<()>` and
//!   `SharedEventSource<()>` provide a convenience `Raise()` overload that
//!   takes no arguments, equivalent to `Raise(())`.
//! cites: 6.5
//! expect: run
//! stdout: ping
module Conf;

public class Clock
  private owned readonly _ping: EventSource<()> := new();
  public property Ping: IEvent<()> => _ping;

  public constructor()
  end constructor;

  public method Fire()
    _ping.Raise();
  end Fire;
end Clock;

public class Program
  public static method Main()
    using var c := new Clock();
    using var sub := c.Ping.Subscribe(lambda(_) => Console.WriteLine("ping"));
    c.Fire();
  end Main;
end Program;
