//! case: s22_15_0008
//! doc: spec
//! section: 22.15
//! title: a move-only value return is rejected on a pure member
//! rule: So is a move-only value return — an owning-choice instantiation,
//!   or an owning inline array — no pure body can produce an owned class
//!   instance or a value that carries one.
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

public abstract class Ops
  public pure abstract method MakeBasket(): Basket;
end Ops;
