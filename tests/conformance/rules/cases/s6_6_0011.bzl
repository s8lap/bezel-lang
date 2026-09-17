//! case: s6_6_0011
//! doc: spec
//! section: 6.6
//! title: a constant shift count outside the left operand's domain is rejected in <<=
//! rule: x <<= k and x >>= k inherit §6.5's count-domain rule through the
//!   desugaring: an out-of-domain count is the same fault — compile error for
//!   a constant count, OverflowException at runtime otherwise.
//! cites: 6.6, 6.5
//! expect: reject
//! diagnostic: count
module Conf;

public class Program
  public static method Main()
    var v: int := 1;
    v <<= 32;
    Console.WriteLine(v);
  end Main;
end Program;
