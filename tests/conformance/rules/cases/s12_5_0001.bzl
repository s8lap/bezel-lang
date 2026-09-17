//! case: s12_5_0001
//! doc: spec
//! section: 12.5
//! title: new on an abstract class is rejected
//! rule: new on an abstract or closed class is a compile error.
//! cites: 12.5
//! expect: reject
//! diagnostic: abstract
module Conf;

public abstract class Shape
  public constructor()
  end constructor;

  public abstract method Area(): double;
end Shape;

public class Program
  public static method Test()
    using var s := new Shape();
  end Test;
end Program;
