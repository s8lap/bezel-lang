//! case: s11_1_0003
//! doc: spec
//! section: 11.1
//! title: a convenience constructor that does not chain to the primary is rejected
//! rule: A class with a primary constructor may also declare additional
//!   constructors in its body using the constructor keyword; such
//!   constructors must chain to the primary constructor via this(...).
//! cites: 11.1
//! expect: reject
//! diagnostic: must chain
module Conf;

public class Widget(name: string)
  private _name: string := name;

  public constructor()
  end constructor;
end Widget;
