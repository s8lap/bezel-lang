//! case: l4_13_0002
//! doc: stdlib
//! section: 4.13
//! title: TakeCurrent before the first advance raises InvalidOperationException
//! rule: A take before the first advance/after exhaustion raises
//!   InvalidOperationException.
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
    try
      _ := it.TakeCurrent();
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
