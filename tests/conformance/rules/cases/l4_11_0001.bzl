//! case: l4_11_0001
//! doc: stdlib
//! section: 4.11
//! title: OwningStack<T>.Pop transfers ownership out without disposing
//! rule: On the owning forms, Pop and Dequeue return ownership; the
//!   collection no longer holds the element.
//! cites: 4.11, 28.11
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
    using var stack := new OwningStack<Widget>();
    stack.Push(new Widget());
    using var w := stack.Pop();
    Console.WriteLine("still alive");
  end Main;
end Program;
