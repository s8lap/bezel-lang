//! case: s25_2_0006
//! doc: spec
//! section: 25.2
//! title: a ref modifier on an operator parameter is rejected
//! rule: Operator parameters admit no parameter modifiers: `ref`, `out`, `owned`
//!   and `scoped` on an operator's parameter list are rejected semantically.
//! cites: 25.2, 22.3
//! expect: reject
//! diagnostic: modifier
module Conf;

public record Weight(Kg: double)
  public static method operator +(ref a: Weight, b: Weight): Weight =>
    new Weight(a.Kg + b.Kg);
end Weight;
