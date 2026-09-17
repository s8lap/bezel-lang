//! case: s11_7_0016
//! doc: spec
//! section: 11.7
//! title: a closed class may not be instantiated directly
//! rule: A class declared with the `closed` modifier is a closed abstract
//!   class: a class that cannot be instantiated directly and cannot be
//!   inherited from outside the declaring module.
//! cites: 11.7, 12.5
//! expect: reject
//! diagnostic: closed
module Conf;

public closed class Shape
  public constructor()
  end constructor;

  public abstract method Area(): double;
end Shape;

public class Program
  public static method Test()
    using var s := new Shape();
  end Test;
end Program;
