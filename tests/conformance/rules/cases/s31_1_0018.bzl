//! case: s31_1_0018
//! doc: spec
//! section: 31.1
//! title: a throw of an owned parameter bounded by a class that does not derive from Exception is rejected
//! rule: "the operand's static type, nullability not admitted, must be"
//! cites: 31.1, 14.2
//! expect: reject
//! diagnostic: must be an exception
module Conf;

public class Plain
  public constructor() end constructor;
end Plain;

public class Program
  public static method M<T>(owned x: T) where T: Plain
    throw x;
  end M;

  public static method Main()
    M(new Plain());
  end Main;
end Program;
