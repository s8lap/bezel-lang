//! case: s22_15_0006
//! doc: spec
//! section: 22.15
//! title: a pure member rejects a move-only value parameter
//! rule: A move-only value parameter — an owning-choice instantiation, or
//!   an owning inline array — is likewise rejected: the plain spelling is
//!   identical to the `owned` spelling for such a type.
//! cites: 22.15, 15.3, 35.4
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
  public static pure method Peek(b: Basket): int => 0;
end Ops;
