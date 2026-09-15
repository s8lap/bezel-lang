//! case: l4_7_0001
//! doc: stdlib
//! section: 4.7
//! title: OwningLinkedList<T>.AddLast takes ownership of its argument
//! rule: method AddLast(owned item: T): LinkedListNode<T>; a fresh
//!   producer into the owning family is idiomatic and warning-free.
//! cites: 4.7, 28.11
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Fill()
    using var list := new OwningLinkedList<Widget>();
    _ := list.AddLast(new Widget());
  end Fill;
end Program;
