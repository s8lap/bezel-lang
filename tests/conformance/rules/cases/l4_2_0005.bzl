//! case: l4_2_0005
//! doc: stdlib
//! section: 4.2
//! title: OwningArray<Widget> is admitted at a class element type
//! rule: The where T: class constraint on the owning family is satisfied
//!   by a concrete class type; a class-typed instantiation is admissible
//!   where a runtime-managed type is not.
//! cites: 4.2, 14.4, 28.11
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    using var pool := new OwningArray<Widget>();
  end Demo;
end Program;
