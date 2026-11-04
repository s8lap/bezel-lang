//! case: s18_5_0023
//! doc: spec
//! section: 18.5
//! title: alignment applies to a unit-typed hole
//! rule: Alignment is an integer, optionally negated; positive right-aligns in a field of that
//!   width, negative left-aligns.
//! cites: 18.5, 8.5, 3.1
//! expect: run
//! output: [    ()]
//!   [()    ]
module Conf;

public class Program
  public static method Main()
    var u := Program.U();
    Console.WriteLine($"[{u,6}]");
    Console.WriteLine($"[{u,-6}]");
  end Main;

  private static method U() end U;
end Program;
