//! case: l9_9_0002
//! doc: stdlib
//! section: 9.9
//! title: NameOf returns the declared constant's own identifier
//! rule: NameOf(value) returns the constant's identifier — under
//!   duplicate values, the first-declared constant's; the pair
//!   round-trips up to aliasing.
//! cites: 9.9
//! expect: run
//! stdout: Red
module Conf;

enum Color: int
  Red := 1, Green := 2;
end Color;

public class Program
  public static method Main()
    Console.WriteLine(Color.NameOf(Color.Red));
  end Main;
end Program;
