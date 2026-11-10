//! case: l2_8_2015
//! doc: stdlib
//! section: 2.8
//! title: DateOnly viewed as IComparable orders chronologically
//! rule: `a.CompareTo(b)` returns a negative number if `a < b`, zero if equal, a positive number if `a > b`.
//! cites: 2.5
//! expect: run
//! output: True
//!   True
//! issue: 7875
module Conf;
public class Program
  public static method Main()
    var first := new DateOnly(1, 1, 1);
    var last := new DateOnly(9999, 12, 31);
    var ifirst: IComparable<DateOnly> := first;
    var ilast: IComparable<DateOnly> := last;
    Console.WriteLine(ifirst.CompareTo(last) < 0);
    Console.WriteLine(ilast.CompareTo(first) > 0);
  end Main;
end Program;
