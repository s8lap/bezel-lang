//! case: s9_2_0010
//! doc: spec
//! section: 9.2
//! title: overriding Equals and GetHashCode disqualifies auto-hashable and equality use is rejected
//! rule: An override of Equals or GetHashCode means the author has taken over
//!   equality; the record is therefore not auto-hashable, and =/<> are a compile
//!   error unless the author also declares the operator =/<> overloads.
//! expect: reject
//! diagnostic: auto-hashable
module Conf;

public record Money
  public readonly property Amount: int;

  public constructor(amount: int)
    Amount := amount;
  end constructor;

  public override method Equals(other: Object?): bool =>
    (if other is Money then true else false);
  public override method GetHashCode(): int => 0;
end Money;

public class Program
  public static method Compare(a: Money, b: Money): bool => a = b;
end Program;
