//! case: s6_5_0001
//! doc: spec
//! section: 6.5
//! title: the bitwise AND operator is not valid on bool
//! rule: Operators &, |, ^, <<, >>, and unary ~ operate on the binary
//!   representation of integer values. They are not valid on bool, char, or
//!   floating-point types.
//! cites: 6.5
//! expect: reject
//! diagnostic: not valid on
module Conf;

public class Program
  public static method Make(a: bool, b: bool)
    var r := a & b;
  end Make;
end Program;
