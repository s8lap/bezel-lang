//! case: s27_1_0001
//! doc: spec
//! section: 27.1
//! title: a two-parameter function type mirrors a two-parameter method declaration
//! rule: A function type is written `(params) -> returnType`; `method Foo(x:
//!   int, y: int): int` parallels `(int, int) -> int`.
//! cites: 27.1
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    var add: (int, int) -> int := lambda(x: int, y: int) => x + y;
    Console.WriteLine($"{add(2, 3)}");
  end Main;
end Program;
