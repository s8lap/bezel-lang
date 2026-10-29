//! case: s8_5_0007
//! doc: spec
//! section: 8.5
//! title: a nullable function-typed local renders its function type or null at an interpolation hole
//! rule: For function values the ToString rendering is the stdlib 2.13 FullName of the
//!   value's own function type, "(int) -> int", parameter names not rendered; a
//!   null hole renders as the four characters null (spec 18.5, 8.5).
//! cites: 18.5, 27.1
//! expect: run
//! output: (int) -> int/null
module Conf;

public class Program
  public static method Main()
    var k := 3;
    var a: ((int) -> int)? := lambda(x: int) => x + k;
    var b: ((int) -> int)? := null;
    Console.WriteLine($"{a}/{b}");
  end Main;
end Program;
