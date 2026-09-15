//! case: l6_5_0002
//! doc: stdlib
//! section: 6.5
//! title: Raise with zero subscribers is a no-op
//! rule: With zero subscribers, both methods are no-ops — no null
//!   check, no ?., no Invoke indirection.
//! cites: 6.5
//! expect: run
//! stdout: ok
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
    t.Fire(1);
    Console.WriteLine("ok");
  end Main;
end Program;
