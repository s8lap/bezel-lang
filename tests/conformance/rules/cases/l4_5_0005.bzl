//! case: l4_5_0005
//! doc: stdlib
//! section: 4.5
//! title: PopFirst takes an element out without disposing it
//! rule: PopFirst(): owned T; // = PopAt(0); raises if empty. Drain
//!   methods transfer ownership of elements out of the collection.
//! cites: 4.5, 28.11
//! expect: run
//! stdout: still alive
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
    using var w := pool.PopFirst();
    Console.WriteLine("still alive");
  end Main;
end Program;
