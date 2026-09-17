//! case: s3_5_0003
//! doc: spec
//! section: 3.5
//! title: bitwise operators on bool operands are a compile error
//! rule: Bitwise (&, |, ^, ~) ... Not valid on bool, char, or floating-point
//!   types. Boolean operations use and/or/not, which short-circuit and have
//!   boolean-only semantics.
//! cites: 3.5
//! expect: reject
//! diagnostic: bitwise
module Conf;

public class Checker
  public static method Combine(): bool => true & false;
end Checker;
