//! case: s9_2_0012
//! doc: spec
//! section: 9.2
//! title: a homogeneous equality overload disqualifies auto-hashable and an undeclared Equals use is rejected
//! rule: A homogeneous operator =/<> overload disqualifies the record from
//!   auto-hashable; calling someRecord.Equals(other) on a non-auto-hashable
//!   record without a user-defined declaration of that member — the
//!   `override` spelling or the plain one — is a compile error. The error
//!   fires per member: Money declares Equals(Money) and not Equals(Object?),
//!   and the Object?-argument call is the member it does not declare.
//! cites: 25.2, 9.2
//! expect: reject
//! diagnostic: auto-hashable
module Conf;

public record Money
  public readonly property Amount: int;

  public constructor(amount: int)
    Amount := amount;
  end constructor;

  public static method operator =(a: Money, b: Money): bool => true;
  public static method operator <>(a: Money, b: Money): bool => false;
  public method Equals(other: Money): bool => true;
  public override method GetHashCode(): int => 0;
end Money;

public class Program
  public static method CallsEquals(a: Money, other: Object?): bool => a.Equals(other);
end Program;
