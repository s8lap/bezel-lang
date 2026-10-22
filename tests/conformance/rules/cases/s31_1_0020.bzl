//! case: s31_1_0020
//! doc: spec
//! section: 31.1
//! title: a throw of an unnarrowed nullable owned operand is rejected
//! rule: "nullability not admitted"
//! cites: 31.1, 7.4
//! expect: reject
//! diagnostic: nullability not admitted
module Conf;

public class Program
  public static method M<T>(owned x: T?) where T: Exception
    throw x;
  end M;

  public static method Main()
    M<IOException>(null);
  end Main;
end Program;
