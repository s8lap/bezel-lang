//! case: l5_7908
//! doc: stdlib
//! section: 5
//! title: Sum with a selector returning a record INumber conformer folds over Zero() and +
//! rule: public method Sum<U>(selector: (T) -> U): owned U where U: INumber<U>;
//! cites: 5, 11.8
//! expect: run
//! output: 11
//!   zab
module Conf;
import System;

public record Money(Cents: int, Label: string): INumber<Money>
  public static method Zero(): Money => new Money(0, "z");
  public static method One(): Money => new Money(1, "o");
  public static method operator +(a: Money, b: Money): Money => new Money(a.Cents + b.Cents, a.Label + b.Label);
  public static method operator -(a: Money, b: Money): Money => new Money(a.Cents - b.Cents, a.Label);
  public static method operator *(a: Money, b: Money): Money => new Money(a.Cents * b.Cents, a.Label);
  public static method operator /(a: Money, b: Money): Money => new Money(a.Cents / b.Cents, a.Label);
  public static method operator <(a: Money, b: Money): bool => a.Cents < b.Cents;
  public static method operator >(a: Money, b: Money): bool => a.Cents > b.Cents;
  public static method operator <=(a: Money, b: Money): bool => a.Cents <= b.Cents;
  public static method operator >=(a: Money, b: Money): bool => a.Cents >= b.Cents;
end Money;

public record Item(Price: Money);

public class Program
  public static method Main()
    var a := new Array<Item>();
    a.Add(new Item(new Money(5, "a")));
    a.Add(new Item(new Money(6, "b")));
    var s := a.Sum(lambda(i) => i.Price);
    Console.WriteLine(s.Cents);
    Console.WriteLine(s.Label);
  end Main;
end Program;
