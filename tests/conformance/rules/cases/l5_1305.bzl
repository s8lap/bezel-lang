//! case: l5_1305
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull is null at int.MaxValue and int.MinValue indexes and accepts a byte index
//! rule: null when index is out of range
//! cites: 15.1
//! expect: run
//! output: -1
//!   -1
//!   7
//!   8
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<int>();
    xs.Add(7);
    xs.Add(8);
    var big := 2147483647;
    Console.WriteLine(xs.Where(lambda(x) => true).ElementAtOrNull(big) ?? -1);
    var neg := -2147483647 - 1;
    Console.WriteLine(xs.Where(lambda(x) => true).ElementAtOrNull(neg) ?? -1);
    Console.WriteLine(xs.Where(lambda(x) => true).ElementAtOrNull(1 - 1) ?? -1);
    var i: byte := 1;
    Console.WriteLine(xs.Where(lambda(x) => true).ElementAtOrNull(i) ?? -1);
  end Main;
end Program;
