//! case: l2_5_4301
//! doc: stdlib
//! section: 2.5
//! title: a DateTime from FromTicks viewed as IComparable and IEquatable orders and equals by ticks
//! rule: public record DateTime: IComparable<DateTime>, IEquatable<DateTime>
//! cites: 2.5, 2.8, 2.9, 5.1
//! expect: run
//! output: True
//!   True
//!   False
//! issue: 8288
module Conf;

public class Program
  public static method Main()
    var a: IComparable<DateTime> := DateTime.FromTicks(100L);
    var b := DateTime.FromTicks(200L);
    Console.WriteLine(a.CompareTo(b) < 0);
    var e: IEquatable<DateTime> := DateTime.FromTicks(100L);
    Console.WriteLine(e.Equals(DateTime.FromTicks(100L)));
    Console.WriteLine(e.Equals(b));
  end Main;
end Program;
