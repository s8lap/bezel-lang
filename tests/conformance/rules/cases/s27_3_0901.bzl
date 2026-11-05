//! case: s27_3_0901
//! doc: spec
//! section: 27.3
//! title: a lambda body of type uint at a (uint) -> long target widens to long and keeps the value
//! rule: `var f: (int) -> long := lambda(x) => x * 2;` is admitted, the `int` body widening to `long`
//! cites: 5.1
//! expect: run
//! output: 4294967295
//! issue: 8159
module Conf;

public class Program
  public static method Main()
    var g: (uint) -> long := lambda(x: uint) => x;
    Console.WriteLine(g(4294967295u));
  end Main;
end Program;
