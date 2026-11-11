//! case: l9_1_4807
//! doc: stdlib
//! section: 9.1
//! title: Array<int>.Sort() orders the full int range from MinValue to MaxValue
//! rule: public record Int32: IComparable<int>, IEquatable<int>
//! cites: 4.4, 14.2
//! expect: run
//! output: -2147483648
//!   -1
//!   0
//!   3
//!   2147483647
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [3, -1, 2147483647, 0, -2147483648];
    a.Sort();
    Console.WriteLine(a[0]);
    Console.WriteLine(a[1]);
    Console.WriteLine(a[2]);
    Console.WriteLine(a[3]);
    Console.WriteLine(a[4]);
  end Main;
end Program;
