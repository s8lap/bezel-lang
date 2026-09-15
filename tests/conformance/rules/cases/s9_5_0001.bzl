//! case: s9_5_0001
//! doc: spec
//! section: 9.5
//! title: with produces a copy with named fields replaced
//! rule: A with expression produces a new record value with named fields
//!   replaced; the original record value is unchanged.
//! expect: run
//! stdout: 1,99
module Conf;

public record Point(X: int, Y: int);

public class Program
  public static method Main()
    var p1 := new Point(1, 2);
    var p2 := p1 with(Y := 99);
    Console.WriteLine($"{p2.X},{p2.Y}");
  end Main;
end Program;
