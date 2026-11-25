//! case: l6_5_9801
//! doc: stdlib
//! section: 6.5
//! title: -= of a handler that was never added is a no-op and leaves the handler usable, 200x
//! rule: `-=` removes the first handler matching by function-value equality (spec §27.7.1) and is a no-op
//!   when no handler matches
//! cites: 6.5, 17.2, 28.9
//! expect: run
//! output: 400
//! notes: Pins #9056 (picks 84423b25f, ccd78de2c): Remove and -= only compare the handler, so the
//!   handler's closure must not be retained or released by them. Neither -= adds anything; the two
//!   direct calls g(1.0) and f(1.0) each hit once, so c.N is 2 per iteration, 400 total, and both
//!   closures stay valid after the no-op removals.
module Conf;

public class Counter2
  private _n: int := 0;
  public method Hit(t: double) => _n := _n + 1;
  public method Ping() => _n := _n + 1;
  public property N: int => _n;
end Counter2;

public class Hub
  private owned readonly _src: EventSource<double> := new EventSource<double>();
  public method Off(handler: (double) -> ()) => _src -= handler;
  public method Fire(t: double) => _src.Raise(t);
  public method Local(c: Counter2)
    var g := lambda(t: double) => c.Hit(t);
    _src -= g;
    _src.Raise(1.0);
    g(1.0);
  end Local;
end Hub;

public class Program
  public static method Main()
    var t := 0;
    for var i in 0..<200 do
      using var c := new Counter2();
      using var hub := new Hub();
      var f := lambda(t: double) => c.Hit(t);
      hub.Off(f);
      hub.Fire(1.0);
      hub.Local(c);
      f(1.0);
      t := t + c.N;
    end;
    Console.WriteLine(t);
  end Main;
end Program;
