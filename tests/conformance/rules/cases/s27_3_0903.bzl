//! case: s27_3_0903
//! doc: spec
//! section: 27.3
//! title: a lambda body of type uint at a (uint) -> decimal target widens to decimal and keeps the value
//! rule: `var f: (int) -> long := lambda(x) => x * 2;` is admitted, the `int` body widening to `long`
//! cites: 5.1
//! expect: run
//! output: 4294967295
//! issue: 8159
module Conf;

public class Program
  public static method Main()
    var h: (uint) -> decimal := lambda(x: uint) => x;
    Console.WriteLine(h(4294967295u));
  end Main;
end Program;
