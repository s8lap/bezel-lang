//! case: l4_2_0002
//! doc: stdlib
//! section: 4.2
//! title: IOwningCollection<T>.Clear disposes every element
//! rule: IOwningCollection<T> declares method Clear(); // disposes every
//!   element — the interface-level contract every owning collection shares.
//! cites: 4.2, 28.11
//! expect: run
//! stdout: widget disposed
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public method ClearIt<T>(target: IOwningCollection<T>) where T: class
  target.Clear();
end ClearIt;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    ClearIt(pool);
    Console.WriteLine("after clear");
  end Main;
end Program;
