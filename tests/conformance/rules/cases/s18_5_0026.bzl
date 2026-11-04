//! case: s18_5_0026
//! doc: spec
//! section: 18.5
//! title: alignment applies to an Object-typed hole holding an int
//! rule: Alignment is an integer, optionally negated; positive right-aligns in a field of that
//!   width, negative left-aligns.
//! cites: 18.5, 8.5, 8.6
//! expect: run
//! output: [    42]
//!   [42    ]
module Conf;

public class Program
  public static method Main()
    var o: Object := 42;
    Console.WriteLine($"[{o,6}]");
    Console.WriteLine($"[{o,-6}]");
  end Main;
end Program;
