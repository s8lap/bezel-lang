//! case: s25_3_0002
//! doc: spec
//! section: 25.3
//! title: a block-bodied operator closed with the wrong token is rejected
//! rule: An `end`-label that is supplied must match the construct it closes; for a
//!   keyword-labelled construct the operator token is part of the label.
//! cites: 25.3, 1.6
//! expect: reject
//! diagnostic: end
module Conf;

public record Vec(X: int)
  public static method operator +(a: Vec, b: Vec): Vec
    return new Vec(a.X + b.X);
  end operator -;
end Vec;
