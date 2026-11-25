//! case: l6_5_9803
//! doc: stdlib
//! section: 6.5
//! title: An AsyncEventSource handler removed with Remove is not invoked by RaiseAsync, 200x
//! rule: // call-bounded: Remove only compares
//! cites: 6.5, 28.14, 28.9
//! expect: run
//! output: 200
//! notes: Pins #9056 (picks 84423b25f, ccd78de2c): Remove and -= only compare the handler, so the
//!   handler's closure must not be retained or released by them. The first RaiseAsync invokes the
//!   handler once, the second (after Off) invokes nothing, and the repeated Remove is a no-op: 1 per
//!   iteration, 200 total.
module Conf;

public class Counter2
  private _n: int := 0;
  public method Hit(t: double) => _n := _n + 1;
  public method Ping() => _n := _n + 1;
  public property N: int => _n;
end Counter2;

public class Hub
  public owned readonly Src: AsyncEventSource<double> := new AsyncEventSource<double>();
  public method On(scoped handler: (double) -> Task) => Src.Add(handler);
  public method Off(handler: (double) -> Task) => Src.Remove(handler);
  public method Fire(t: double): Task => Src.RaiseAsync(t);
end Hub;

public class Program
  public static async method Main(): Task<int>
    var t := 0;
    for var i in 0..<200 do
      using var c := new Counter2();
      using var hub := new Hub();
      var f: (double) -> Task := async lambda(t: double) => do c.Hit(t); end;
      hub.On(f);
      await hub.Fire(1.0);
      hub.Off(f);
      await hub.Fire(1.0);
      hub.Src.Remove(f);
      t := t + c.N;
    end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
