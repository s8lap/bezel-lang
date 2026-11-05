//! case: s18_5_2004
//! doc: spec
//! section: 18.5
//! title: a record implementing IFormattable dispatches ToString(format)
//!   and a format hole to its member
//! rule: a **user** type opts in by implementing the auto-imported
//!   interface `IFormattable`
//! cites: 2.18, 8.5
//! expect: run
//! output: M5:Q
//!   M5:Q
module Conf;

public record Money(Cents: int): IFormattable
  public method ToString(format: string): string => $"M{Cents}:{format}";
end Money;

public class Program
  public static method Main()
    var m := new Money(5);
    Console.WriteLine(m.ToString("Q"));
    Console.WriteLine($"{m:Q}");
  end Main;
end Program;
