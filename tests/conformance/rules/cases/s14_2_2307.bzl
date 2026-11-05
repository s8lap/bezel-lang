//! case: s14_2_2307
//! doc: spec
//! section: 14.2
//! title: Math.Max, Min and Clamp over the fixed-width numeric primitives
//!   are accepted
//! rule: A primitive conforms to the interfaces its standard-library
//!   declaration lists
//! cites: 2.2, 11.8
//! expect: run
//! output: 9
//!   3
//!   2.5
//!   -3
//!   3
//!   3
//! notes: Accepting control (green on base and tip): every numeric
//!   primitive declares IComparable in its stdlib declaration.
module Conf;

public class Program
  public static method Main()
    var a: byte := 3;
    var b: byte := 9;
    Console.WriteLine(Math.Max(a, b));
    var c: uint := 3;
    var d: uint := 9;
    Console.WriteLine(Math.Min(c, d));
    var e: float := 1.5f;
    var f: float := 2.5f;
    Console.WriteLine(Math.Max(e, f));
    var g: sbyte := -3;
    Console.WriteLine(Math.Clamp(g, g, g));
    var h: ulong := 3;
    Console.WriteLine(Math.Max(h, h));
    var i: short := 3;
    Console.WriteLine(Math.Max(i, i));
  end Main;
end Program;
