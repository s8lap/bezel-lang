//! case: s25_2_0007
//! doc: spec
//! section: 25.2
//! title: an owned modifier on an operator parameter is rejected
//! rule: Operands are received as plain borrows (or value copies) under their type
//!   kinds; the return slot's optional `owned` is the sole ownership annotation on
//!   operators.
//! cites: 25.2, 28.1
//! expect: reject
//! diagnostic: modifier
module Conf;

public class Buffer
  public constructor()
  end constructor;
end Buffer;

public record Pair(N: int)
  public static method operator +(a: Pair, owned b: Buffer): Pair => a;
end Pair;
