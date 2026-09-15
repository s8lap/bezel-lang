//! case: s22_15_0020
//! doc: spec
//! section: 22.15
//! title: a pure body may not invoke a function-typed value
//! rule: Prohibited: invoking function-typed values (the value's type
//!   carries no effect information).
//! cites: 22.15
//! expect: reject
//! diagnostic: invoking
module Conf;

public class Ops
  public static pure method Apply(f: (int) -> int, x: int): int => f(x);
end Ops;
