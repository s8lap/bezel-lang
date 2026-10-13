//! case: s9_2_0027
//! doc: spec
//! section: 9.2
//! title: a record declaring GetHashCode and no Equals rejects a direct Equals call
//! rule: Calling `someRecord.Equals(other)` or `someRecord.GetHashCode()` on a
//!   non-auto-hashable record without a user-defined declaration of that
//!   member — the `override` spelling or the plain one — is a compile error
//!   ... The error is per member
//! cites: 9.2
//! expect: reject
//! diagnostic: auto-hashable
module Conf;

public record Money
  public readonly property Amount: int;

  public constructor(amount: int)
    Amount := amount;
  end constructor;

  public method GetHashCode(): int => Amount;
end Money;

public class Program
  public static method Same(a: Money, b: Money): bool => a.Equals(b);
end Program;
