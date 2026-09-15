//! case: s25_1_0005
//! doc: spec
//! section: 25.1
//! title: an operator overload declared without static is rejected
//! rule: Operators are declared as `public static` methods using the `operator`
//!   keyword.
//! cites: 25
//! expect: reject
//! diagnostic: static
module Conf;

public record Weight(Kg: double)
  public method operator +(a: Weight, b: Weight): Weight =>
    new Weight(a.Kg + b.Kg);
end Weight;
