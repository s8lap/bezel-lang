//! case: s27_7_0003
//! doc: spec
//! section: 27.7
//! title: the result of a null-conditional call through a present function value is nullable
//! rule: The null-conditional call result is nullable.
//! cites: 27.7
//! expect: run
//! stdout: 36
module Conf;

public class Program
  public static method Main()
    var maybeSquare: ((int) -> int)? := lambda(x: int) => x * x;
    var result := maybeSquare?(6);
    Console.WriteLine($"{result}");
  end Main;
end Program;
