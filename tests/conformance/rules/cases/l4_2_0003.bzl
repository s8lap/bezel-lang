//! case: l4_2_0003
//! doc: stdlib
//! section: 4.2
//! title: IOwningCollection<T>.Remove disposes the removed element
//! rule: IOwningCollection<T> declares method Remove(item: T): bool; //
//!   disposes the removed element — the borrow-taking query removes and
//!   disposes what it finds.
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

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    var w := new Widget();
    pool.Add(move w);
    pool.Remove(pool[0]);
    Console.WriteLine("after remove");
  end Main;
end Program;
