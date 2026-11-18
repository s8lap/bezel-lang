//! case: l4_6_7306
//! doc: stdlib
//! section: 4.6
//! title: the `.` spelling of FirstNode and LastNode on a LinkedList still
//!   reads the head and tail nodes
//! rule: public pure readonly property FirstNode: LinkedListNode<T>?;
//! cites: 7.4
//! expect: run
//! output: False
//!   5
//!   False
//! notes: Admitting twin of the `?.` cases: the plain `.` property read on a
//!   non-null list receiver. The node is nullable, so its own member access
//!   is made through `!`.
module Conf;

public class Program
  public static method Main()
    using var l := new LinkedList<int>();
    var a := l.AddLast(5);
    var n := l.FirstNode;
    Console.WriteLine(n = null);
    Console.WriteLine(n!.Value);
    Console.WriteLine(l.LastNode = null);
  end Main;
end Program;
