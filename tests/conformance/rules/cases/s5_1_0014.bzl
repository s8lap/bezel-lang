//! case: s5_1_0014
//! doc: spec
//! section: 5.1
//! title: a compile-time-constant int narrows implicitly to byte when its value fits
//! rule: An expression of integer type that is a compile-time constant
//!   implicitly converts to any integer type whose range contains its value:
//!   var b: byte := 50; is exactly such a conversion.
//! cites: 5.1
//! expect: accept
module Conf;

public class Program
  public static method Make()
    var b: byte := 50;
  end Make;
end Program;
