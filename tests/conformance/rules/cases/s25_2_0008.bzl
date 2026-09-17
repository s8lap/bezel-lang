//! case: s25_2_0008
//! doc: spec
//! section: 25.2
//! title: an operator whose parameters never mention the declaring type is rejected
//! rule: At least one parameter of an operator overload must be the declaring type;
//!   an operator whose parameters never mention it would be unreachable by the
//!   resolution rule.
//! cites: 25.2
//! expect: reject
//! diagnostic: declaring type
module Conf;

public record Tag(N: int)
  public static method operator +(a: int, b: int): int => a + b;
end Tag;
