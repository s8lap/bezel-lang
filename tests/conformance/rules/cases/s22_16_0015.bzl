//! case: s22_16_0015
//! doc: spec
//! section: 22.16
//! title: a move-only value operand is a compile error at old
//! rule: A move-only value operand is a compile error at the contract
//!   clause: a copy is the operation move-only-ness forbids, and the
//!   literal desugaring would not copy but consume.
//! cites: 22.16, 15.3, 35.4
//! expect: reject
//! diagnostic: move-only
module Conf;

public class Box
  public constructor()
  end constructor;
end Box;

public choice Basket
  Full(owned item: Box),
  Empty;
end Basket;

public class Ops
  public static method Peek(b: Basket): int
    ensures((case old(b) of Full(_) => true, Empty => false))

    return 0;
  end Peek;
end Ops;
