//! case: l4_3_0005
//! doc: stdlib
//! section: 4.3
//! title: Drain() is the route from the owning family to ownership-transferring iteration
//! rule: For draining iteration that transfers ownership, the owning
//!   collections expose Drain() returning IOwningEnumerable<T>; default
//!   iteration stays borrowing, and the owning path is taken only at
//!   static type IOwningEnumerable<T>.
//! cites: 4.3, 4.5, 28.11, 29.2
//! expect: run
//! stdout: shutting down
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Shutdown()
    Console.WriteLine("shutting down");
  end Shutdown;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    for var w in pool.Drain() do
      using var owned_w := move w;
      owned_w.Shutdown();
    end;
  end Main;
end Program;
