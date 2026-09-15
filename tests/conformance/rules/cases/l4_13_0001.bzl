//! case: l4_13_0001
//! doc: stdlib
//! section: 4.13
//! title: a second TakeCurrent without an intervening advance raises InvalidOperationException
//! rule: A second take without an intervening advance, or a take before the
//!   first advance/after exhaustion, raises InvalidOperationException.
//! cites: 4.13, 29.1
//! expect: run
//! stdout: caught
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    using var view := pool.Drain();
    using var it := view.GetOwningEnumerator();
    _ := it.MoveNext();
    using var w := it.TakeCurrent();
    try
      _ := it.TakeCurrent();
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
