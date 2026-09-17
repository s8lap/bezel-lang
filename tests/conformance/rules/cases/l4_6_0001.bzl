//! case: l4_6_0001
//! doc: stdlib
//! section: 4.6
//! title: LinkedList<T>.AddLast returns a node borrowing the list
//! rule: method AddLast(item: T): LinkedListNode<T>; LinkedListNode<T>
//!   instances are owned by their containing LinkedList<T>; methods
//!   returning LinkedListNode<T> return a borrow whose lifetime is the
//!   list's lifetime.
//! cites: 4.6
//! expect: run
//! stdout: 7
module Conf;

public class Program
  public static method Main()
    using var list := new LinkedList<int>();
    var node := list.AddLast(7);
    Console.WriteLine(node.Value);
  end Main;
end Program;
