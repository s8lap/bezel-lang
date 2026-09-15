//! case: l4_11_0006
//! doc: stdlib
//! section: 4.11
//! title: OwningStack<T>.Peek borrows the top element without removing it
//! rule: pure method Peek(): T; // borrow of top element — for class T on
//!   the owning forms, Pop and Dequeue transfer ownership, while Peek
//!   only borrows.
//! cites: 4.11, 28.11
//! expect: run
//! stdout: 1
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var stack := new OwningStack<Widget>();
    stack.Push(new Widget());
    var top := stack.Peek();
    Console.WriteLine(stack.Count);
  end Main;
end Program;
