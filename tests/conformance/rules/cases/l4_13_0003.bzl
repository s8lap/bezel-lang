//! case: l4_13_0003
//! doc: stdlib
//! section: 4.13
//! title: an advanced-but-untaken element is disposed when the enumerator is disposed
//! rule: Between a successful advance and TakeCurrent() the enumerator owns the
//!   pending value -- a Dispose() after an advance whose value was never taken
//!   disposes it; an advanced-but-untaken element neither leaks nor reaches
//!   the consumer.
//! cites: 4.13, 29.1
//! expect: run
//! stdout: disposed
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    do
      using var view := pool.Drain();
      using var it := view.GetOwningEnumerator();
      _ := it.MoveNext();
    end;
    Console.WriteLine("after");
  end Main;
end Program;
