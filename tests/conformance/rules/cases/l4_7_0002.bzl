//! case: l4_7_0002
//! doc: stdlib
//! section: 4.7
//! title: OwningLinkedList<T>.RemoveFirst disposes the removed element
//! rule: RemoveFirst(); // disposes — the owning list's removal methods
//!   dispose the element they remove.
//! cites: 4.7, 28.11
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
    using var list := new OwningLinkedList<Widget>();
    _ := list.AddLast(new Widget());
    list.RemoveFirst();
    Console.WriteLine("after removefirst");
  end Main;
end Program;
