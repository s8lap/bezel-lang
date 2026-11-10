//! case: l2_8_2012
//! doc: stdlib
//! section: 2.8
//! title: DateTime satisfies a T: IComparable<T> constraint and the constrained CompareTo orders by ticks
//! rule: `a.CompareTo(b)` returns a negative number if `a < b`, zero if equal, a positive number if `a > b`.
//! cites: 2.5
//! expect: run
//! output: True
//!   True
module Conf;
public class Program
  static method Larger<T>(a: T, b: T): T where T: IComparable<T>
    if a.CompareTo(b) >= 0 then
      return a;
    end if;
    return b;
  end Larger;

  public static method Main()
    var early := new DateTime(2020, 1, 2, 3, 4, 5);
    var later := new DateTime(2020, 1, 2, 3, 4, 6);
    Console.WriteLine(Larger(early, later).Ticks = later.Ticks);
    Console.WriteLine(Larger(later, early).Ticks = later.Ticks);
  end Main;
end Program;
