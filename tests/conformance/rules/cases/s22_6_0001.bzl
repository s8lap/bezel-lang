//! case: s22_6_0001
//! doc: spec
//! section: 22.6
//! title: two overloads indistinguishable by arity, types, modifiers clash
//! rule: A type may not declare two members indistinguishable under
//!   §22.6's rules — the clash rejected at the second declaration.
//! cites: 22.6, 11.3
//! expect: reject
//! diagnostic: indistinguishable
module Conf;

public class Calculator
  public method Compute(a: int): int => a;
  public method Compute(a: int): int => a * 2;
end Calculator;
