//! case: l6_5_0008
//! doc: stdlib
//! section: 6.5
//! title: disposing the Subscription removes the handler
//! rule: Subscribe registers a handler and returns an owned Subscription;
//!   disposing the subscription removes the handler.
//! cites: 2.11
//! expect: run
//! output: handled
//!   done
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
    do
      using var sub := t.Tick.Subscribe(lambda(_) => Console.WriteLine("handled"));
      t.Fire(1);
    end;
    t.Fire(2);
    Console.WriteLine("done");
  end Main;
end Program;
