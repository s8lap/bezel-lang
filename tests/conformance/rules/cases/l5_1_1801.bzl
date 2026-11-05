//! case: l5_1_1801
//! doc: stdlib
//! section: 5.1
//! title: Sum over a skipped sequence folds a user INumber record from its Zero and operator plus
//! rule: an accumulator over `U.Zero()` and `+`, exactly spec §11.8's own `Sum<T>`
//!   example
//! cites: 5, 11.8, 14.2
//! expect: run
//! output: 1300
//!   zbc
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
  static method H<T>(a: Array<T>): T where T: INumber<T> => a.Skip(1).Sum();
  public static method Main()
    var a := new Array<Money>();
    a.Add(new Money(5, "a")); a.Add(new Money(6, "b")); a.Add(new Money(7, "c"));
    var i := 0;
    var t := 0;
    while i < 100 do
      t := t + H(a).Cents;
      i := i + 1;
    end while;
    Console.WriteLine(t);
    Console.WriteLine(H(a).Label);
  end Main;
end Program;
