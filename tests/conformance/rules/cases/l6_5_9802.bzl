//! case: l6_5_9802
//! doc: stdlib
//! section: 6.5
//! title: A handler removed with Remove is not invoked by Raise but remains callable directly, 200x
//! rule: // call-bounded: Remove only compares
//! cites: 6.5, 28.14, 28.9
//! expect: run
//! output: 400
//! notes: Pins #9056 (picks 84423b25f, ccd78de2c): Remove and -= only compare the handler, so the
//!   handler's closure must not be retained or released by them. After Remove the handler is no
//!   longer registered, so Fire invokes nothing; the two direct calls hit twice: c.N is 2 per
//!   iteration, 400 total. Remove only compares, so f stays valid.
module Conf;

public class Counter2
  private _n: int := 0;
  public method Hit(t: double) => _n := _n + 1;
  public method Ping() => _n := _n + 1;
  public property N: int => _n;
end Counter2;

public class Hub
  public owned readonly Src: EventSource<double> := new EventSource<double>();
  public method On(scoped handler: (double) -> ()) => Src.Add(handler);
  public method Fire(t: double) => Src.Raise(t);
end Hub;

public class Program
  public static method Main()
    var t := 0;
    for var i in 0..<200 do
      using var c := new Counter2();
      using var hub := new Hub();
      var f := lambda(t: double) => c.Hit(t);
      hub.On(f);
      hub.Src.Remove(f);
      f(1.0);
      hub.Fire(1.0);
      f(1.0);
      t := t + c.N;
    end;
    Console.WriteLine(t);
  end Main;
end Program;
