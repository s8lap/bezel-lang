//! case: l2_5_4304
//! doc: stdlib
//! section: 2.5
//! title: a TimeOnly from Parse viewed as IComparable and IEquatable orders and equals by time
//! rule: public record TimeOnly: IComparable<TimeOnly>, IEquatable<TimeOnly>
//! cites: 2.5, 2.8, 2.9, 5.1
//! expect: run
//! output: True
//!   True
//!   False
//! issue: 8288
module Conf;

public class Program
  public static method Main()
    var a: IComparable<TimeOnly> := TimeOnly.Parse("10:00:00");
    var b := TimeOnly.Parse("09:00:00");
    Console.WriteLine(a.CompareTo(b) > 0);
    var e: IEquatable<TimeOnly> := TimeOnly.Parse("10:00:00");
    Console.WriteLine(e.Equals(TimeOnly.Parse("10:00:00")));
    Console.WriteLine(e.Equals(b));
  end Main;
end Program;
