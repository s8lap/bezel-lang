//! case: s22_8_0007
//! doc: spec
//! section: 22.8
//! title: a field initializer may not reference this or an instance member
//! rule: "Field initializers may **not** reference: `this`, instance
//!   members, `base`, virtual methods, or any expression that would
//!   observe `this` indirectly."
//! cites: 22.8
//! expect: reject
//! diagnostic: this
module Conf;

public class Widget
  private _base: int := 10;
  private _derived: int := this._base;

  public constructor()
  end constructor;
end Widget;
