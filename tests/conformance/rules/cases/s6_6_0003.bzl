//! case: s6_6_0003
//! doc: spec
//! section: 6.6
//! title: a read-modify-write compound assignment on a nullable operand is a compile error
//! rule: A read-modify-write compound assignment is a compile error when the
//!   static type of its left-hand side, or of its right operand, is a
//!   nullable type — the desugared x op e has no meaning on a nullable
//!   operand.
//! cites: 6.6, 7.11
//! expect: reject
//! diagnostic: no meaning
module Conf;

public class Program
  public static method Make(m: int?)
    m += 1;
  end Make;
end Program;
