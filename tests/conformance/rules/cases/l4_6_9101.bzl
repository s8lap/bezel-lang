//! case: l4_6_9101
//! doc: stdlib
//! section: 4.6
//! title: LinkedList First/Last at element slots and FirstNode at a node slot are admitted
//! rule: public pure readonly property First: T?;
//! cites: 4.6, 5.1
//! expect: run
//! output: 4
//!   9
//!   4
//!   null
//! notes: Pins the #8757 family (pick ed5af90fa): a LinkedList<T> element or node read stored at a
//!   LinkedListNode<T> slot of the wrong type. Spec-decided admitting twin of s5_1_9101..9104:
//!   the element read keeps the element type (`int?`), the node property keeps the node type,
//!   and `null` is a legal node-slot value; every conversion is identity or the 5.1 nullable
//!   lift. Green on the base too.
module Conf;

public class Program
  public static method Main(): int
    using var m := new LinkedList<int>();
    m.AddLast(4);
    m.AddLast(9);
    var a: int? := m.First;
    var b: int? := m.Last;
    var n: LinkedListNode<int>? := m.FirstNode;
    var z: LinkedListNode<int>? := null;
    Console.WriteLine($"{a ?? -1}");
    Console.WriteLine($"{b ?? -1}");
    Console.WriteLine($"{n!.Value}");
    if z = null then Console.WriteLine("null"); end if;
    return 0;
  end Main;
end Program;
