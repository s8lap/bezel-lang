//! case: s27_6_9402
//! doc: spec
//! section: 27.6
//! title: a bare overloaded method name resolves against an annotated function type
//! rule: When the named method (or property) is overloaded, the reference is resolved using the
//!   **target function type** at the use site.
//! cites: 27.6
//! expect: run
//! output: 99
//!   11
module Conf;

public class Program
  static method P(i: int): int => i + 10;
  static method P(s: string): int => 99;
  public static method Main(): int
    var f: (string) -> int := P;
    Console.WriteLine(f("x"));
    var g: (int) -> int := P;
    Console.WriteLine(g(1));
    return 0;
  end Main;
end Program;
