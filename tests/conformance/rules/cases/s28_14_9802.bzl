//! case: s28_14_9802
//! doc: spec
//! section: 28.14
//! title: An if-expression of two scoped parameters may be forwarded to Add, 200x
//! rule: forward it to another `scoped` parameter whose bounding object is the same object
//! cites: 28.14, 6.5, 28.9
//! expect: run
//! output: 200
//! notes: Pins #9056 (picks 84423b25f, ccd78de2c): Remove and -= only compare the handler, so the
//!   handler's closure must not be retained or released by them. Either branch is a scoped parameter
//!   of the same bounding object, so the conditional forwards to Add; the one chosen handler fires
//!   once, the removals are no-op compares: 1 per iteration, 200 total.
module Conf;

public class Counter2
  private _n: int := 0;
  public method Hit(t: double) => _n := _n + 1;
  public method Ping() => _n := _n + 1;
  public property N: int => _n;
end Counter2;

public class Hub
  public owned readonly Src: EventSource<double> := new EventSource<double>();
  public method On(scoped a: (double) -> (), scoped b: (double) -> (), pick: bool) => Src.Add((if pick then a else b));
  public method Fire(t: double) => Src.Raise(t);
end Hub;

public class Program
  public static method Main()
    var t := 0;
    for var i in 0..<200 do
      using var c := new Counter2();
      using var hub := new Hub();
      var f := lambda(t: double) => c.Hit(t);
      var g := lambda(t: double) => c.Ping();
      hub.On(f, g, i mod 2 = 0);
      hub.Fire(1.0);
      hub.Src.Remove(f);
      hub.Src.Remove(g);
      hub.Fire(1.0);
      t := t + c.N;
    end;
    Console.WriteLine(t);
  end Main;
end Program;
