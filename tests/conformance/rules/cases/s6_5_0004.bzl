//! case: s6_5_0004
//! doc: spec
//! section: 6.5
//! title: a compile-time-constant shift count outside the left operand's width domain is a compile error
//! rule: A count outside the domain — negative, or >= width — is a fault: a
//!   compile error when the count is a compile-time constant.
//! cites: 6.5
//! expect: reject
//! diagnostic: outside the domain
module Conf;

public class Program
  public static method Make()
    const F := 1 << 64;
  end Make;
end Program;
