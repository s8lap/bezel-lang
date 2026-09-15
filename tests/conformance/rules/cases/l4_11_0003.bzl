//! case: l4_11_0003
//! doc: stdlib
//! section: 4.11
//! title: OwningQueue<T>.Clear disposes every element
//! rule: OwningStack<T>/OwningQueue<T>.Clear(); // disposes every
//!   element.
//! cites: 4.11, 28.11
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

public class Program
  public static method Main()
    using var queue := new OwningQueue<Widget>();
    queue.Enqueue(new Widget());
    queue.Clear();
    Console.WriteLine("after clear");
  end Main;
end Program;
