//! case: s17_2_9801
//! doc: spec
//! section: 17.2
//! title: A handler added and then removed with += and -= on a local lambda is invoked only while added, 200x
//! rule: `lhs -= h` to `lhs.Remove(h)`
//! cites: 17.2, 28.14, 28.9
//! expect: run
//! output: 200
//! notes: Pins #9056 (picks 84423b25f, ccd78de2c): Remove and -= only compare the handler, so the
//!   handler's closure must not be retained or released by them. Raise before -= invokes g once,
//!   Raise after it invokes nothing: c.N is 1 each iteration, 200 total.
module Conf;

public class Counter2
  private _n: int := 0;
  public method Hit(t: double) => _n := _n + 1;
  public method Ping() => _n := _n + 1;
  public property N: int => _n;
end Counter2;

public class Hub
  private owned readonly _src: EventSource<double> := new EventSource<double>();
  public method Run(c: Counter2)
    var g := lambda(t: double) => c.Hit(t);
    _src += g;
    _src.Raise(1.0);
    _src -= g;
    _src.Raise(1.0);
  end Run;
end Hub;

public class Program
  public static method Main()
    var t := 0;
    for var i in 0..<200 do
      using var c := new Counter2();
      using var hub := new Hub();
      hub.Run(c);
      t := t + c.N;
    end;
    Console.WriteLine(t);
  end Main;
end Program;
