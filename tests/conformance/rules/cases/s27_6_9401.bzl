//! case: s27_6_9401
//! doc: spec
//! section: 27.6
//! title: a bare overloaded method name with no target type is ambiguous
//! rule: When no target type is available (e.g., `var f := Math.Abs;` with no annotation), the
//!   reference is ambiguous and the compiler reports an error pointing to the candidate
//!   overloads.
//! cites: 27.6
//! expect: reject
//! diagnostic: overloaded
module Conf;

public class Program
  static method P(i: int): int => i + 10;
  static method P(s: string): int => 99;
  public static method Main(): int
    var f := P;
    Console.WriteLine(f(1));
    return 0;
  end Main;
end Program;
