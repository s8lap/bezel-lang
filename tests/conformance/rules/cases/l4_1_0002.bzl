//! case: l4_1_0002
//! doc: stdlib
//! section: 4.1
//! title: Array<T>.Remove removes the borrow without disposing the element
//! rule: ICollection<T>'s Remove/Clear/RemoveAt on a borrowing collection
//!   remove without disposing; the element's holder, if any, keeps its
//!   disposal obligation.
//! cites: 28.11
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
    using var arr := new Array<Widget>();
    arr.Add(w);
    arr.Remove(w);
    Console.WriteLine("still alive");
  end Main;
end Program;
