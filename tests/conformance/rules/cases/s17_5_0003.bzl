//! case: s17_5_0003
//! doc: spec
//! section: 17.5
//! title: a nullable left operand over a range test is rejected
//! rule: "This covers every range test: stdlib §9.8 declares `Contains(value:
//!   T)` over the non-nullable `T` ... so `n in 1..5` over an `int?` is
//!   rejected."
//! cites: 17.5, 16.3
//! expect: reject
//! diagnostic: implicitly convert
module Conf;

public class Program
  public static method Check(n: int?): bool
    var r := n in 1..5;
    return r;
  end Check;
end Program;
