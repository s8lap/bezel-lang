//! case: l4_7_0003
//! doc: stdlib
//! section: 4.7
//! title: OwningLinkedList<T>.PopFirst transfers ownership out without disposing
//! rule: To take an element out, use RemoveNode (which disposes), PopFirst
//!   / PopLast (transfer ownership to caller), or Drain().
//! cites: 4.7, 28.11
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
    using var list := new OwningLinkedList<Widget>();
    _ := list.AddLast(new Widget());
    using var w := list.PopFirst();
    Console.WriteLine("still alive");
  end Main;
end Program;
