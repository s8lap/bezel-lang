//! case: l6_5_0007
//! doc: stdlib
//! section: 6.5
//! title: a handler subscribing during a raise affects only the next raise
//! rule: Raise invokes handlers over a snapshot of the subscriber list taken
//!   when the raise begins (v0.163): a handler that subscribes or
//!   unsubscribes during the raise affects the next raise, never the
//!   in-flight pass.
//! cites: 6.5
//! expect: run
//! output: mid
//!   late
module Conf;

public class Bus
  private owned readonly _e: EventSource<int> := new();
  public property E: IEvent<int> => _e;

  public constructor()
  end constructor;

  public method AddLate()
    _e += lambda(_) => Console.WriteLine("late");
  end AddLate;

  public method Fire(n: int)
    _e.Raise(n);
  end Fire;
end Bus;

public class Program
  public static method Main()
    using var b := new Bus();
    using var sub := b.E.Subscribe(lambda(_) => b.AddLate());
    b.Fire(1);
    Console.WriteLine("mid");
    b.Fire(2);
  end Main;
end Program;
