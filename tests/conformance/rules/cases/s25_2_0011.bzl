//! case: s25_2_0011
//! doc: spec
//! section: 25.2
//! title: the co-declaration rule is satisfied on a record by a plain own-typed Equals and a plain GetHashCode
//! rule: A record, enum, or choice that overloads `=`/`<>` with both operands
//!   of the declaring type must also declare `Equals(T)` and `GetHashCode` —
//!   on a record, an enum, or a choice alike (`override` or plain on each
//! cites: 25.2, 9.2, 8.3
//! expect: accept
module Conf;

public record Money
  public readonly property Amount: int;

  public constructor(amount: int)
    Amount := amount;
  end constructor;

  public static method operator =(a: Money, b: Money): bool => a.Amount = b.Amount;
  public static method operator <>(a: Money, b: Money): bool => a.Amount <> b.Amount;
  public method Equals(other: Money): bool => Amount = other.Amount;
  public method GetHashCode(): int => Amount;
end Money;

public class Program
  public static method Same(a: Money, b: Money): bool => a = b;
end Program;
