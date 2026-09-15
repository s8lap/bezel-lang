//! case: l4_7_0004
//! doc: stdlib
//! section: 4.7
//! title: OwningLinkedList<T> has no populate-from-source constructor
//! rule: There is no source constructor (v0.265): OwningLinkedList<T>
//!   declares constructor() and no populate-from-source form, deliberately
//!   — a borrowing source has no elements to give.
//! cites: 4.7
//! expect: reject
//! diagnostic: constructor
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    using var src: Array<Widget> := [];
    using var list := new OwningLinkedList<Widget>(src);
  end Demo;
end Program;
