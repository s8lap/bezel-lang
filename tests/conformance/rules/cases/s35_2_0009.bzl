//! case: s35_2_0009
//! doc: spec
//! section: 35.2
//! title: an override Equals taking Object? on a choice is rejected
//! rule: and an `Equals` taking `Object?` or `Object` rejected in either
//!   spelling, at every classification, as §10.5 rejects it on an enum
//! cites: 35.2, 10.5, 8.3
//! expect: reject
//! diagnostic: equals
//! issue: 5610
module Conf;

public choice Shape
  Circle(radius: int),
  Empty;

  public override method Equals(o: Object?): bool => false;
  public override method GetHashCode(): int => (case this of Circle(var r) => r, Empty => 0);
end Shape;
