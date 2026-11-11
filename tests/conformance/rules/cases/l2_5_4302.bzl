//! case: l2_5_4302
//! doc: stdlib
//! section: 2.5
//! title: a TimeSpan from Zero and FromTicks viewed as IComparable and IEquatable orders and equals
//! rule: public record TimeSpan: IComparable<TimeSpan>, IEquatable<TimeSpan>
//! cites: 2.5, 2.8, 2.9, 5.1
//! expect: run
//! output: True
//!   True
//!   False
//! issue: 8288
module Conf;

public class Program
  public static method Main()
    var a: IComparable<TimeSpan> := TimeSpan.Zero;
    var b := TimeSpan.FromTicks(5L);
    Console.WriteLine(a.CompareTo(b) < 0);
    var e: IEquatable<TimeSpan> := TimeSpan.FromTicks(5L);
    Console.WriteLine(e.Equals(b));
    Console.WriteLine(e.Equals(TimeSpan.Zero));
  end Main;
end Program;
