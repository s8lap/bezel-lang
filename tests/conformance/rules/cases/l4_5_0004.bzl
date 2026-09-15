//! case: l4_5_0004
//! doc: stdlib
//! section: 4.5
//! title: OwningArray<T>.Clear disposes every element
//! rule: Clear(); // disposes every element — the array's own Dispose()
//!   cascades the same way.
//! cites: 4.5, 28.11
//! expect: run
//! output: widget disposed
//!   widget disposed
//!   after clear
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    pool.Add(new Widget());
    pool.Clear();
    Console.WriteLine("after clear");
  end Main;
end Program;
