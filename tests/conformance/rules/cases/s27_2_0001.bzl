//! case: s27_2_0001
//! doc: spec
//! section: 27.2
//! title: parentheses around a function type are required before a trailing question mark
//! rule: The parentheses around the function type are required before `?`;
//!   `((int) -> bool)?` is a nullable function returning bool, callable with
//!   the null-conditional call form.
//! cites: 27.2, 27.7
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var check: ((int) -> bool)? := lambda(x: int) => x > 0;
    var result := check?(5);
    Console.WriteLine($"{result}");
  end Main;
end Program;
