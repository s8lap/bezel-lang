//! case: s6_1_0007
//! doc: spec
//! section: 6.1
//! title: mod is not defined on float or double
//! rule: mod is not defined on float or double; an expression of the form x
//!   mod y where either operand has a binary floating-point type is a compile
//!   error.
//! cites: 6.1
//! expect: reject
//! diagnostic: binary floating-point
module Conf;

public class Program
  public static method Make(x: double, y: double)
    var r := x mod y;
  end Make;
end Program;
