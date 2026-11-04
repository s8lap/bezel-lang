//! case: s18_5_0024
//! doc: spec
//! section: 18.5
//! title: alignment applies to a positional-record hole
//! rule: Alignment is an integer, optionally negated; positive right-aligns in a field of that
//!   width, negative left-aligns.
//! cites: 18.5, 8.5, 9.1
//! expect: run
//! output: [      Pt(X: 1, Y: 2)]
//!   [Pt(X: 1, Y: 2)      ]
module Conf;
public record Pt(X: int, Y: int);

public class Program
  public static method Main()
    var p := new Pt(1, 2);
    Console.WriteLine($"[{p,20}]");
    Console.WriteLine($"[{p,-20}]");
  end Main;
end Program;
