//! case: s18_5_0019
//! doc: spec
//! section: 18.5
//! title: alignment applies to a function-valued hole, right and left
//! rule: Alignment is an integer, optionally negated; positive right-aligns in a field of that
//!   width, negative left-aligns.
//! cites: 18.5, 8.5, 27.1
//! expect: run
//! output: [  (int) -> int]
//!   [(int) -> int  ]
module Conf;

public class Program
  public static method Main()
    var g: (int) -> int := lambda(x: int) => x;
    Console.WriteLine($"[{g,14}]");
    Console.WriteLine($"[{g,-14}]");
  end Main;
end Program;
