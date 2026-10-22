//! case: s31_1_0019
//! doc: spec
//! section: 31.1
//! title: a throw of an owned parameter with no bound is rejected
//! rule: "the operand's static type, nullability not admitted, must be"
//! cites: 31.1, 14.2
//! expect: reject
//! diagnostic: must be an exception
module Conf;

public class Program
  public static method M<T>(owned x: T)
    throw x;
  end M;

  public static method Main()
    M(new IOException("m"));
  end Main;
end Program;
