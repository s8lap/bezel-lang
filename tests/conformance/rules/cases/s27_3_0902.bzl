//! case: s27_3_0902
//! doc: spec
//! section: 27.3
//! title: a lambda body of type byte at a (byte) -> decimal target widens to decimal and keeps the value
//! rule: `var f: (int) -> long := lambda(x) => x * 2;` is admitted, the `int` body widening to `long`
//! cites: 5.1
//! expect: run
//! output: 200
//! issue: 8159
module Conf;

public class Program
  public static method Main()
    var f: (byte) -> decimal := lambda(x: byte) => x;
    Console.WriteLine(f(200));
  end Main;
end Program;
