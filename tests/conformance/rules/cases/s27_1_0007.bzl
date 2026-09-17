//! case: s27_1_0007
//! doc: spec
//! section: 27.1
//! title: a call through a function-typed value takes positional arguments
//! rule: A call through a function-typed value admits no named argument
//!   (v0.295); calls through a function-typed value are positional.
//! cites: 27.1, 22.5
//! expect: run
//! stdout: 98
module Conf;

public class Program
  public static method Main()
    var caller: (a: int, b: int) -> int := lambda(x: int, y: int) => x - y;
    Console.WriteLine($"{caller(100, 2)}");
  end Main;
end Program;
