//! case: s10_5_0009
//! doc: spec
//! section: 10.5
//! title: an override Equals taking Object? on an enum is rejected
//! rule: An `Equals` taking `Object?` or `Object` is rejected on an enum at
//!   the declaration, as on every value kind (§9.2), spelled `override` or
//!   plain, the diagnostic naming the own-typed spelling
//! cites: 10.5, 8.1, 8.3, 8.6
//! expect: reject
//! diagnostic: equals
//! issue: 5610
module Conf;

enum Priority
  Low, Medium, High;

  public override method Equals(o: Object?): bool => false;
  public override method GetHashCode(): int => int(this);
end Priority;
