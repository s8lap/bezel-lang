//! case: l4_6_0002
//! doc: stdlib
//! section: 4.6
//! title: LinkedList<T>.RemoveFirst removes without disposing the element
//! rule: LinkedList<T> is a borrowing collection (ICollection<T>);
//!   RemoveFirst/RemoveLast/RemoveNode remove without disposing, per the
//!   borrowing-family rule (spec §28.11).
//! cites: 4.6, 28.11
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
    using var w := new Widget();
    using var list := new LinkedList<Widget>();
    _ := list.AddLast(w);
    list.RemoveFirst();
    Console.WriteLine("still alive");
  end Main;
end Program;
