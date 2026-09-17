//! case: l4_11_0002
//! doc: stdlib
//! section: 4.11
//! title: Stack<T>.Clear drops every element and disposes nothing
//! rule: The borrowing forms also provide Clear(): it drops every element
//!   reference and disposes nothing, per the borrowing-family rule.
//! cites: 4.11, 28.11
//! expect: run
//! output: 0
//!   still alive
//!   widget disposed
//!   widget disposed
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
    using var w2 := new Widget();
    using var stack := new Stack<Widget>();
    stack.Push(w);
    stack.Push(w2);
    stack.Clear();
    Console.WriteLine(stack.Count);
    Console.WriteLine("still alive");
  end Main;
end Program;
