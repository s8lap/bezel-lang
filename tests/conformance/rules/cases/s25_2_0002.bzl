//! case: s25_2_0002
//! doc: spec
//! section: 25.2
//! title: a homogeneous equality overload with Equals and GetHashCode beside it
//! rule: A record, enum or choice that overloads `=`/`<>` with both operands of
//!   the declaring type must also declare `Equals(T)` and `GetHashCode`.
//! cites: 25.2, 9.2
//! expect: accept
module Conf;

public record Money(Amount: decimal)
  public static method operator =(a: Money, b: Money): bool => a.Amount = b.Amount;
  public static method operator <>(a: Money, b: Money): bool => a.Amount <> b.Amount;

  public method Equals(other: Money): bool => Amount = other.Amount;
  public override method GetHashCode(): int => Amount.GetHashCode();
end Money;
