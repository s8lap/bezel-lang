//! case: s26_7_0001
//! doc: spec
//! section: 26.7
//! title: an extension cannot return this as owned
//! rule: The receiver is always a borrow; extensions cannot store `this`,
//!   return it as `owned`, or dispose it.
//! cites: 26.7
//! expect: reject
//! diagnostic: owned
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public extension Widget
  public method Keep(): owned Widget => this;
end;
