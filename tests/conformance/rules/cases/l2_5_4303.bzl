//! case: l2_5_4303
//! doc: stdlib
//! section: 2.5
//! title: a DateOnly from Parse viewed as IComparable and IEquatable orders and equals by date
//! rule: public record DateOnly: IComparable<DateOnly>, IEquatable<DateOnly>
//! cites: 2.5, 2.8, 2.9, 5.1
//! expect: run
//! output: True
//!   True
//!   False
//! issue: 8288
module Conf;

public class Program
  public static method Main()
    var a: IComparable<DateOnly> := DateOnly.Parse("2020-01-02");
    var b := DateOnly.Parse("2021-01-02");
    Console.WriteLine(a.CompareTo(b) < 0);
    var e: IEquatable<DateOnly> := DateOnly.Parse("2020-01-02");
    Console.WriteLine(e.Equals(DateOnly.Parse("2020-01-02")));
    Console.WriteLine(e.Equals(b));
  end Main;
end Program;
