//! case: s8_3_0001
//! doc: spec
//! section: 8.3
//! title: overriding Equals without GetHashCode is rejected
//! rule: A type that overrides `Equals` must also override `GetHashCode`.
//!   The compiler enforces this.
//! cites: 8.3, C.1
//! expect: reject
//! diagnostic: gethashcode
module Conf;

public class Money
  private _cents: int;

  public constructor(cents: int)
    _cents := cents;
  end constructor;

  public override method Equals(other: Object?): bool =>
    (if other is Money then (other as Money)._cents = _cents else false);
end Money;
