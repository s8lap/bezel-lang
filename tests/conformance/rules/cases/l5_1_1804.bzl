//! case: l5_1_1804
//! doc: stdlib
//! section: 5.1
//! title: Sum through nested generic bodies folds a record and leaves the int aggregates unchanged
//! rule: an accumulator over `U.Zero()` and `+`, exactly spec §11.8's own `Sum<T>`
//!   example
//! cites: 5, 11.8, 14.2
//! expect: run
//! output: 11
//!   6
//!   8
//!   4
//!   12
//! issue: 8223
module Conf;

public record Money(Cents: int, Label: string): INumber<Money>
  public static method Zero(): Money => new Money(0, "z");
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
  static method K<U>(a: Array<U>): U where U: INumber<U> => a.Sum();
  static method H<T>(a: Array<T>): T where T: INumber<T> => K(a);
  public static method Main()
    var a := new Array<Money>();
    a.Add(new Money(5, "a")); a.Add(new Money(6, "b"));
    Console.WriteLine(H(a).Cents);
    var i := new Array<int>(); i.Add(4); i.Add(8);
    Console.WriteLine(i.Average());
    Console.WriteLine(i.Max());
    Console.WriteLine(i.Min());
    Console.WriteLine(H(i));
  end Main;
end Program;
