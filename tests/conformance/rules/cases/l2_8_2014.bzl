//! case: l2_8_2014
//! doc: stdlib
//! section: 2.8
//! title: IComparable orders the extreme DateTime values correctly
//! rule: `a.CompareTo(b)` returns a negative number if `a < b`, zero if equal, a positive number if `a > b`.
//! cites: 2.5
//! expect: run
//! output: True
//!   True
//! issue: 7875
module Conf;
public class Program
  public static method Main()
    var lo := new DateTime(1, 1, 1);
    var hi := new DateTime(9999, 12, 31, 23, 59, 59);
    var ilo: IComparable<DateTime> := lo;
    var ihi: IComparable<DateTime> := hi;
    Console.WriteLine(ilo.CompareTo(hi) < 0);
    Console.WriteLine(ihi.CompareTo(lo) > 0);
  end Main;
end Program;
