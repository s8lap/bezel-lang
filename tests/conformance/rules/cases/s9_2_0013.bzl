//! case: s9_2_0013
//! doc: spec
//! section: 9.2
//! title: declaring Equals(T) beside a homogeneous equality overload makes the direct call legal
//! rule: The compile error on a non-auto-hashable record covers Equals(T) and
//!   GetHashCode; it fires per member and a user-defined declaration of that
//!   member — the `override` spelling or the plain one — removes it for that
//!   member. (An Equals taking Object? is rejected on a record, §9.2.)
//! cites: 25.2, 9.2
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
    Console.WriteLine(a.Equals(b));
  end Main;
end Program;
