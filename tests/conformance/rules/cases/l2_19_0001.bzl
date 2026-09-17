//! case: l2_19_0001
//! doc: stdlib
//! section: 2.19
//! title: a user record conforming to INumber<T> is usable through the
//!   constraint
//! rule: User types conform to INumber<T> by declaring and supplying the
//!   statics; at a value conformer the operator witnesses are unmarked,
//!   matching the erased signatures.
//! cites: 2.19
//! expect: run
//! stdout: 400
module Conf;

public record Money(Cents: int): INumber<Money>
  public static method Zero(): Money => new Money(0);
  public static method One(): Money => new Money(1);
  public static method operator +(a: Money, b: Money): Money => new Money(a.Cents + b.Cents);
  public static method operator -(a: Money, b: Money): Money => new Money(a.Cents - b.Cents);
  public static method operator *(a: Money, b: Money): Money => new Money(a.Cents * b.Cents);
  public static method operator /(a: Money, b: Money): Money => new Money(a.Cents / b.Cents);
  public static method operator <(a: Money, b: Money): bool => a.Cents < b.Cents;
end Money;

public class Program
  public static method Sum<T>(a: T, b: T): T where T: INumber<T> => a + b;

  public static method Main()
    var total := Sum(new Money(150), new Money(250));
    Console.WriteLine(total.Cents);
  end Main;
end Program;
