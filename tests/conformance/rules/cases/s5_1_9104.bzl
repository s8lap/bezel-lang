//! case: s5_1_9104
//! doc: spec
//! section: 5.1
//! title: an `int` constant stored at a `LinkedListNode<int>?` slot is rejected
//! rule: The following conversions are implicit, and the term *implicit conversion* throughout this
//!   document
//! cites: 5.1, 12.4
//! expect: reject
//! diagnostic: implicit
//! issue: 8757
//! notes: Pins the #8757 family (pick ed5af90fa): a LinkedList<T> element or node read stored at a
//!   LinkedListNode<T> slot of the wrong type. A value type does not convert to the
//!   compiler-known node class.
module Conf;

public class Program
  public static method Main(): int
    var n: LinkedListNode<int>? := 5;
    return 0;
  end Main;
end Program;
