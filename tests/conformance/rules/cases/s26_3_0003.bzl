//! case: s26_3_0003
//! doc: spec
//! section: 26.3
//! title: an extension block may not declare an operator overload
//! rule: An extension block declares no operator overloads.
//! cites: 26.3, 25.1
//! expect: reject
//! diagnostic: operator
module Conf;

public record Weight(Kg: double);

extension Weight
  public static method operator +(a: Weight, b: Weight): Weight =>
    new Weight(a.Kg + b.Kg);
end;
