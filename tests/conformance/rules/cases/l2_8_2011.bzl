//! case: l2_8_2011
//! doc: stdlib
//! section: 2.8
//! title: a DateTime passed as an IComparable argument orders by tick order, in both directions
//! rule: `a.CompareTo(b)` returns a negative number if `a < b`, zero if equal, a positive number if `a > b`.
//! cites: 2.5
//! expect: run
//! output: True
//!   True
//!   True
//! issue: 7875
module Conf;
public class Program
  static method Cmp(x: IComparable<DateTime>, y: DateTime): int => x.CompareTo(y);

  public static method Main()
    var early := new DateTime(2020, 1, 2, 3, 4, 5);
    var later := new DateTime(2020, 1, 2, 3, 4, 6);
    Console.WriteLine(Cmp(early, later) < 0);
    Console.WriteLine(Cmp(later, early) > 0);
    Console.WriteLine(Cmp(later, later) = 0);
  end Main;
end Program;
