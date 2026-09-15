//! case: s35_3_0008
//! doc: spec
//! section: 35.3
//! title: constructing a holds-borrows choice from a temporary is rejected
//! rule: An argument to a plain class- or interface-typed payload slot is a borrow, and
//!   §9.7's no-temporary-source rule applies verbatim: constructing a choice that
//!   holds borrows from a temporary bound to a borrow-carrying payload is
//!   rejected.
//! expect: reject
//! diagnostic: temporary
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public choice Holding
  Some(widget: Widget),
  None;
end Holding;

public class Program
  public static method Main()
    var h := Holding.Some(new Widget());   // ERROR: fresh temporary bound to a borrow-carrying payload
  end Main;
end Program;
