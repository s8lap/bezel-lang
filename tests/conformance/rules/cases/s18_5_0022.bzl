//! case: s18_5_0022
//! doc: spec
//! section: 18.5
//! title: alignment applies to an inline-array hole, right and left
//! rule: Alignment is an integer, optionally negated; positive right-aligns in a field of that
//!   width, negative left-aligns.
//! cites: 18.5, 8.5, 15.1
//! expect: run
//! output: [    [2]int]
//!   [[2]int    ]
module Conf;

public class Program
  public static method Main()
    var a: [2]int := [1, 2];
    Console.WriteLine($"[{a,10}]");
    Console.WriteLine($"[{a,-10}]");
  end Main;
end Program;
