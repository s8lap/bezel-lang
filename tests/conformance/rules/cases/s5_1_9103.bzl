//! case: s5_1_9103
//! doc: spec
//! section: 5.1
//! title: a LinkedList element read passed to a `LinkedListNode<Item>?` parameter is rejected
//! rule: The following conversions are implicit, and the term *implicit conversion* throughout this
//!   document
//! cites: 5.1, 12.4
//! expect: reject
//! diagnostic: implicit
//! issue: 8757
//! notes: Pins the #8757 family (pick ed5af90fa): a LinkedList<T> element or node read stored at a
//!   LinkedListNode<T> slot of the wrong type. The parameter position of the same slot (the
//!   parameter is nullable so the nullability drop of 7.1 is not a second ground).
module Conf;

public class Item
  public constructor() end constructor;
end Item;

public class Program
  public static method Take(n: LinkedListNode<Item>?)
  end Take;

  public static method Main(): int
    using var l := new LinkedList<Item>();
    Take(l.First);
    return 0;
  end Main;
end Program;
