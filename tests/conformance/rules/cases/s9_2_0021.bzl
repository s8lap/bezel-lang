//! case: s9_2_0021
//! doc: spec
//! section: 9.2
//! title: an override Equals at Object? on a record is rejected at the declaration
//! rule: An `Equals` taking `Object?` or `Object` is rejected on every value
//!   kind at the declaration, `override` or plain, the diagnostic naming the
//!   own-typed spelling
//! cites: 9.2, 8.3
//! expect: reject
//! diagnostic: Equals
//! issue: 5924
module Conf;

public record Money
  public readonly property Amount: int;

  public constructor(amount: int)
    Amount := amount;
  end constructor;

  public override method Equals(other: Object?): bool => other is Money;
  public override method GetHashCode(): int => Amount;
end Money;
