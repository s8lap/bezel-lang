//! case: s28_14_9801
//! doc: spec
//! section: 28.14
//! title: A user Remove(scoped handler) that stores into a scoped field may invoke it later, 200x
//! rule: but only in `scoped` fields **of the bounding object itself** (below)
//! cites: 28.14, 28.9
//! expect: run
//! output: 200
//! notes: Pins #9056 (picks 84423b25f, ccd78de2c): Remove and -= only compare the handler, so the
//!   handler's closure must not be retained or released by them. Control: a user class's method that
//!   merely happens to be named Remove is not the call-bounded event-source Remove; it stores into a
//!   scoped field and Fire invokes it: 1 per iteration, 200 total.
module Conf;

public class Counter2
  private _n: int := 0;
  public method Hit(t: double) => _n := _n + 1;
  public method Ping() => _n := _n + 1;
  public property N: int => _n;
end Counter2;

public class Bag
  private scoped _h: ((double) -> ())?;
  public method Remove(scoped handler: (double) -> ()) => _h := handler;
  public method Fire(t: double)
    _ := _h?(t);
  end Fire;
end Bag;

public class Program
  public static method Main()
    var t := 0;
    for var i in 0..<200 do
      using var c := new Counter2();
      using var b := new Bag();
      var f := lambda(x: double) => c.Hit(x);
      b.Remove(f);
      b.Fire(1.0);
      t := t + c.N;
    end;
    Console.WriteLine(t);
  end Main;
end Program;
