//! case: l4_4_0048
//! doc: stdlib
//! section: 4.4
//! title: Array LastIndexOf returns the last occurrence on bool, long, double and short arrays, or -1
//! rule: public method LastIndexOf(item: T): int;      // last occurrence, or -1.
//! cites: 4.4
//! expect: run
//! output: 2
//!   1
//!   2
//!   1
//!   -1
module Conf;

public class Program
  public static method Main()
    var b: Array<bool> := [true, false, true, false];
    var l: Array<long> := [1L, 2L, 1L, 3L];
    var d: Array<double> := [1.5, 2.5, 1.5];
    var s: Array<short> := [short(1), short(2), short(1)];
    Console.WriteLine(b.LastIndexOf(true));
    Console.WriteLine(l.LastIndexOf(2L));
    Console.WriteLine(d.LastIndexOf(1.5));
    Console.WriteLine(s.LastIndexOf(short(2)));
    Console.WriteLine(l.LastIndexOf(9L));
  end Main;
end Program;
