//! case: l2_8_0003
//! doc: stdlib
//! section: 2.8
//! title: CompareTo's sign convention drives a where T: IComparable<T> caller
//! rule: a.CompareTo(b) returns negative if a < b, zero if equal, positive if
//!   a > b; a generic caller constrained on IComparable<T>, such as Math.Min,
//!   relies on this convention to select the lesser value.
//! cites: 2.2
//! expect: run
//! stdout: 5
module Conf;

public record Money(Amount: int): IComparable<Money>
  public method CompareTo(other: Money): int =>
    (if Amount < other.Amount then -1 elif Amount > other.Amount then 1 else 0);
end Money;

public class Program
  public static method Main()
    var lower := Math.Min(new Money(5), new Money(10));
    Console.WriteLine(lower.Amount);
  end Main;
end Program;
