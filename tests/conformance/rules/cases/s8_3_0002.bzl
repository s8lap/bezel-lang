//! case: s8_3_0002
//! doc: spec
//! section: 8.3
//! title: overriding Equals together with GetHashCode is admitted
//! rule: A type that overrides `Equals` must also override `GetHashCode`.
//!   Declaring both together is admitted.
//! cites: 8.3, C.1
//! expect: accept
module Conf;

public class Money
  private _cents: int;

  public constructor(cents: int)
    _cents := cents;
  end constructor;

  public override method Equals(other: Object?): bool =>
    (if other is Money then (other as Money)._cents = _cents else false);

  public override method GetHashCode(): int => _cents;
end Money;
