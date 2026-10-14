//! case: s27_5_0012
//! doc: spec
//! section: 27.5
//! title: a nested lambda keeps reading its enclosing lambda's parameter after that lambda has returned
//! rule: A lambda may reference variables from its enclosing scope. Captured
//!   variables are heap-allocated in a closure object, so the closure outlives
//!   the call that formed it. (v0.338 §27.5)
//! cites: 27.5
//! expect: run
//! output: 30
//!   40
//!   30
module Conf;

public class Program
  public static method Main()
    var mk := lambda(x: int) => lambda() => x * 10;
    var a := mk(3);
    var b := mk(4);
    Console.WriteLine(a());
    Console.WriteLine(b());
    Console.WriteLine(a());
  end Main;
end Program;
