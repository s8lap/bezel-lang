//! case: s27_7_0001
//! doc: spec
//! section: 27.7
//! title: a function-typed value is called with ordinary call syntax
//! rule: A function-typed value is called with ordinary call syntax:
//!   `var sum := add(2, 3);`.
//! cites: 27.7
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    var add: (int, int) -> int := lambda(x: int, y: int) => x + y;
    var sum := add(2, 3);
    Console.WriteLine($"{sum}");
  end Main;
end Program;
