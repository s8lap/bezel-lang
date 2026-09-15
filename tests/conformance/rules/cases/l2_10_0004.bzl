//! case: l2_10_0004
//! doc: stdlib
//! section: 2.10
//! title: where T: IDisposable admits no record
//! rule: The constraint where T: IDisposable admits every class, no records
//!   -- records cannot implement IDisposable.
//! cites: 9.7
//! expect: reject
//! diagnostic: IDisposable
module Conf;

public record Money(Amount: int);

public static class Helpers
  public static method Accepts<T>(x: T): bool where T: IDisposable => true;
end Helpers;

public class Program
  public static method Main()
    Console.WriteLine(Helpers.Accepts(new Money(5)));
  end Main;
end Program;
