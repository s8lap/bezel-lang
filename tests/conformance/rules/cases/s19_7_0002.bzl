//! case: s19_7_0002
//! doc: spec
//! section: 19.7
//! title: an expression-position case atom composes inside an interpolation hole
//! rule: The atom is self-delimited and composes inside any expression
//!   position, including inside string interpolations; it opens with
//!   `(case` and its closing parenthesis is the terminator — there is no
//!   `end`.
//! cites: 19.7
//! expect: run
//! stdout: grade: B
module Conf;

public class Program
  public static method Main()
    var score := 85;
    Console.WriteLine($"grade: {(case score of 90..100 => "A", 70..<90 => "B", _ => "F")}");
  end Main;
end Program;
