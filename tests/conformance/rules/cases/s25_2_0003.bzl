//! case: s25_2_0003
//! doc: spec
//! section: 25.2
//! title: a homogeneous equality overload without Equals and GetHashCode is rejected
//! rule: The co-declaration rule is compiler-enforced: the three surfaces (`=`,
//!   `Equals`, hashing) can otherwise silently diverge exactly where it hurts —
//!   `a = b` true while a `Set<T>` treats them as distinct.
//! cites: 25.2, 9.2
//! expect: reject
//! diagnostic: Equals
module Conf;

public record Money(Amount: decimal)
  public static method operator =(a: Money, b: Money): bool => a.Amount = b.Amount;
  public static method operator <>(a: Money, b: Money): bool => a.Amount <> b.Amount;
end Money;
