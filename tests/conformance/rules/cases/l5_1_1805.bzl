//! case: l5_1_1805
//! doc: stdlib
//! section: 5.1
//! title: Sum folds with the homogeneous operator plus when a heterogeneous overload is declared before it
//! rule: an accumulator over `U.Zero()` and `+`, exactly spec §11.8's own `Sum<T>`
//!   example
//! cites: 11.8, 14.2
//! expect: run
//! output: 11
//!   zab
//! issue: 8223
module Conf;

public record Money(Cents: int, Label: string): INumber<Money>
  public static method Zero(): Money => new Money(0, "z");
  public static method operator +(a: Money, b: int): Money => new Money(a.Cents + b + 1000, a.Label);
  public static method operator +(a: Money, b: Money): Money => new Money(a.Cents + b.Cents, a.Label + b.Label);
  public static method One(): Money => new Money(1, "o");
  public static method operator -(a: Money, b: Money): Money => new Money(a.Cents - b.Cents, a.Label);
  public static method operator *(a: Money, b: Money): Money => new Money(a.Cents * b.Cents, a.Label);
  public static method operator /(a: Money, b: Money): Money => new Money(a.Cents / b.Cents, a.Label);
  public static method operator <(a: Money, b: Money): bool => a.Cents < b.Cents;
  public static method operator >(a: Money, b: Money): bool => a.Cents > b.Cents;
  public static method operator <=(a: Money, b: Money): bool => a.Cents <= b.Cents;
  public static method operator >=(a: Money, b: Money): bool => a.Cents >= b.Cents;
end Money;
public class Program
  static method H<T>(a: Array<T>): T where T: INumber<T> => a.Sum();
  public static method Main()
    var a := new Array<Money>();
    a.Add(new Money(5, "a")); a.Add(new Money(6, "b"));
    var s := H(a);
    Console.WriteLine(s.Cents);
    Console.WriteLine(s.Label);
  end Main;
end Program;
