//! case: l2_5_4801
//! doc: stdlib
//! section: 2.5
//! title: Array<DateTime>.Sort() orders by tick order: the earlier instant first
//! rule: public record DateTime: IComparable<DateTime>, IEquatable<DateTime>
//! cites: 4.4, 14.2
//! expect: run
//! output: 100
//!   300
module Conf;

public class Program
  public static method Main()
    var a: Array<DateTime> := [DateTime.FromUnixTimeSeconds(300L), DateTime.FromUnixTimeSeconds(100L)];
    a.Sort();
    Console.WriteLine(a[0].ToUnixTimeSeconds());
    Console.WriteLine(a[1].ToUnixTimeSeconds());
  end Main;
end Program;
