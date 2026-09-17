//! case: s9_1_0002
//! doc: spec
//! section: 9.1
//! title: the auto-generated positional properties read back the constructor arguments
//! rule: A record may declare positional fields in parentheses after its name; the
//!   compiler generates a primary constructor whose parameters are the positional
//!   fields and public read-only properties with the same names.
//! expect: run
//! stdout: 3,4
module Conf;

public record Point(X: int, Y: int);

public class Program
  public static method Main()
    var p := new Point(3, 4);
    Console.WriteLine($"{p.X},{p.Y}");
  end Main;
end Program;
