//! case: s5_1_9101
//! doc: spec
//! section: 5.1
//! title: a LinkedList element read (`int?`) stored at a `LinkedListNode<int>?` slot is rejected
//! rule: The following conversions are implicit, and the term *implicit conversion* throughout this
//!   document
//! cites: 5.1, 12.4
//! expect: reject
//! diagnostic: implicit
//! issue: 8757
//! notes: Pins the #8757 family (pick ed5af90fa): a LinkedList<T> element or node read stored at a
//!   LinkedListNode<T> slot of the wrong type. stdlib 4.6 types `First` as `T?`, so the
//!   initializer is `int?`; no row of 5.1 converts an `int` to the compiler-known class
//!   LinkedListNode<int>. Accepting twin: l4_6_9101 (the node property at the node slot).
module Conf;

public class Program
  public static method Main(): int
    using var m := new LinkedList<int>();
    var n: LinkedListNode<int>? := m.First;
    return 0;
  end Main;
end Program;
