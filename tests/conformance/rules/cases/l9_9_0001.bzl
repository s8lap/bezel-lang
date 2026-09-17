//! case: l9_9_0001
//! doc: stdlib
//! section: 9.9
//! title: FromUnderlyingOrNull returns null for an undeclared underlying value
//! rule: It returns the declared constant whose underlying value equals
//!   value, or null when there is none — total, never raises.
//! cites: 9.9
//! expect: run
//! stdout: null
module Conf;

enum Color: int
  Red := 1, Green := 2;
end Color;

public class Program
  public static method Main()
    Console.WriteLine($"{Color.FromUnderlyingOrNull(99)}");
  end Main;
end Program;
