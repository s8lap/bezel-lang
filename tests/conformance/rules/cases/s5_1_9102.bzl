//! case: s5_1_9102
//! doc: spec
//! section: 5.1
//! title: a LinkedList `Last` element read (`Item?`) stored at a `LinkedListNode<Item>?` slot is
//!   rejected
//! rule: The following conversions are implicit, and the term *implicit conversion* throughout this
//!   document
//! cites: 5.1, 12.4
//! expect: reject
//! diagnostic: implicit
//! issue: 8757
//! notes: Pins the #8757 family (pick ed5af90fa): a LinkedList<T> element or node read stored at a
//!   LinkedListNode<T> slot of the wrong type. The element read is `Item?`; the compiler-known
//!   class LinkedListNode<T> is closed to inheritance (12.4), so Item is not a subclass of it
//!   and no class-to-base conversion exists.
module Conf;

public class Item
  public constructor() end constructor;
end Item;

public class Program
  public static method Main(): int
    using var l := new LinkedList<Item>();
    var n: LinkedListNode<Item>? := l.Last;
    return 0;
  end Main;
end Program;
