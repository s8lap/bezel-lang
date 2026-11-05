//! case: s14_2_2309
//! doc: spec
//! section: 14.2
//! title: Math.Max and Clamp with explicit numeric type arguments are
//!   accepted
//! rule: A primitive conforms to the interfaces its standard-library
//!   declaration lists
//! cites: 2.2
//! expect: run
//! output: 4
//!   3
//! notes: Accepting control (green on base and tip).
module Conf;

public class Program
  public static method Main()
    var m := Math.Max<int>(3, 4);
    var n := Math.Clamp<long>(5L, 1L, 3L);
    Console.WriteLine(m);
    Console.WriteLine(n);
  end Main;
end Program;
