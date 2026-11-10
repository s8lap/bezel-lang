//! case: l2_8_2016
//! doc: stdlib
//! section: 2.8
//! title: TimeOnly viewed as IComparable orders by time of day
//! rule: `a.CompareTo(b)` returns a negative number if `a < b`, zero if equal, a positive number if `a > b`.
//! cites: 2.5
//! expect: run
//! output: True
//!   True
//! issue: 7875
module Conf;
public class Program
  public static method Main()
    var midnight := new TimeOnly(0, 0);
    var evening := new TimeOnly(23, 59);
    var imid: IComparable<TimeOnly> := midnight;
    var ieve: IComparable<TimeOnly> := evening;
    Console.WriteLine(imid.CompareTo(evening) < 0);
    Console.WriteLine(ieve.CompareTo(midnight) > 0);
  end Main;
end Program;
