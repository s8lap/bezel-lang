//! case: s9_2_0011
//! doc: spec
//! section: 9.2
//! title: declaring the operator overloads beside an Equals or GetHashCode override restores equality use
//! rule: = and <> on a record outside the auto-hashable set are a compile error
//!   unless the author also declares the operator =/<> overloads, along with
//!   Equals(T) and GetHashCode overrides as needed.
//! cites: 25.2
//! expect: run
//! stdout: True
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
  public static method Main()
    var a := new Money(1);
    var b := new Money(999);
    Console.WriteLine(a = b);
  end Main;
end Program;
