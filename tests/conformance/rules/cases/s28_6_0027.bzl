//! case: s28_6_0027
//! doc: spec
//! section: 28.6
//! title: a method returning this without owned is admitted
//! rule: A method that returns `this` returns a borrow of the receiver — the
//!   return type is the class type, not `owned`. This enables method
//!   chaining.
//! cites: 28.6
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Self(): Widget => this;
end Widget;
