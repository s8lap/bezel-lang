//! case: l2_8_2013
//! doc: stdlib
//! section: 2.8
//! title: DateTime values stored as IComparable elements of an Array compare by ticks
//! rule: `a.CompareTo(b)` returns a negative number if `a < b`, zero if equal, a positive number if `a > b`.
//! cites: 2.5
//! expect: run
//! output: True
//!   True
//! issue: 7875
module Conf;
public class Program
  public static method Main()
    var early := new DateTime(2020, 1, 2, 3, 4, 5);
    var later := new DateTime(2020, 1, 2, 3, 4, 6);
    var arr: Array<IComparable<DateTime>> := [early, later];
    Console.WriteLine(arr[1].CompareTo(early) > 0);
    Console.WriteLine(arr[0].CompareTo(later) < 0);
  end Main;
end Program;
