//! case: s27_3_0911
//! doc: spec
//! section: 27.3
//! title: lambda bodies widen short to int and a constant narrows int to short
//! rule: its inferred return type must equal the target's return slot or implicitly convert to it
//!   ... implicitly converts to any integer type whose range contains its value
//! cites: 5.1
//! expect: run
//! output: 3
//!   5
//! issue: 8168
module Conf;

public class Program
  public static method Main()
    var f: (short) -> int := lambda(x) => x;
    var g: (int) -> short := lambda(x) => 5;
    Console.WriteLine(f(3));
    Console.WriteLine(g(1));
  end Main;
end Program;
