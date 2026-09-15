//! case: l4_14_0008
//! doc: stdlib
//! section: 4.14
//! title: a slot moved-from via move buf[i] must be restored before the enclosing scope exits
//! rule: move buf[i] leaves the slot moved-from; the field-restoration rule
//!   of language spec 28.8.1 applies -- any slot moved-from must be
//!   reassigned before any exit edge, because auto-dispose on a moved-from
//!   slot is not defined.
//! cites: 4.14, 28.8
//! expect: reject
//! diagnostic: moved-from
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<Widget>(1);
    buf[0] := new Widget();
    using var w := move buf[0];
  end Main;
end Program;
