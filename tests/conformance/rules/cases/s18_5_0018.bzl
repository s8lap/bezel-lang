//! case: s18_5_0018
//! doc: spec
//! section: 18.5
//! title: nested interpolated strings render function-typed holes and null holes
//! rule: A null hole renders as the four characters null; a function value renders as
//!   its function type FullName (spec 8.5, 18.5); an interpolated string is itself
//!   usable as a hole expression.
//! cites: 8.5
//! expect: run
//! output: [(int) -> int-null]
module Conf;

public class Program
  public static method Main()
    var k := 4;
    var a: ((int) -> int)? := lambda(x: int) => x + k;
    var b: ((int) -> int)? := null;
    Console.WriteLine($"[{$"{a}-{b}"}]");
  end Main;
end Program;
