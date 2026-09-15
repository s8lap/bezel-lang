//! case: s17_10_0003
//! doc: spec
//! section: 17.10
//! title: typeof(this) is a compile error
//! rule: "`typeof(this)` and `typeof(base)` are compile errors. `this` and
//!   `base` are values, not type names."
//! cites: 17.10
//! expect: reject
//! diagnostic: value
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Describe(): Type => typeof(this);
end Widget;
