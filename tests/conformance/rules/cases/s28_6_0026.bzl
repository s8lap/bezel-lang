//! case: s28_6_0026
//! doc: spec
//! section: 28.6
//! title: a method returning this under an owned return type is rejected
//! rule: A method that returns `this` returns a borrow of the receiver. The
//!   returned borrow may not be assigned to an `owned` field or returned as
//!   `owned` — it is the receiver under a different name.
//! cites: 28.6
//! expect: reject
//! diagnostic: borrow
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Self(): owned Widget => this;
end Widget;
