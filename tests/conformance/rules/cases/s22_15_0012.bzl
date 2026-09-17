//! case: s22_15_0012
//! doc: spec
//! section: 22.15
//! title: pure is rejected on an auto-property as redundant
//! rule: Fields and auto-properties as declarations: the auto-accessor
//!   classification is implicit; writing `pure` on an auto-property is
//!   rejected as redundant.
//! cites: 22.15
//! expect: reject
//! diagnostic: redundant
module Conf;

public class Box2
  public pure property Value: int;

  public constructor(v: int)
    Value := v;
  end constructor;
end Box2;
