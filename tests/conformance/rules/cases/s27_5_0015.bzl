//! case: s27_5_0015
//! doc: spec
//! section: 27.5
//! title: several nested closures built from one enclosing parameter share a single outer local
//! rule: Captured variables are heap-allocated in a closure object.
//!   Modifications to a captured variable inside the lambda are visible
//!   outside, and vice versa. (v0.338 §27.5)
//! cites: 27.5
//! expect: run
//! output: 7
module Conf;

public class Program
  public static method Main()
    var total := 0;
    var add := lambda(n: int) => lambda() => do
      total := total + n;
    end;
    var f1 := add(2);
    var f2 := add(3);
    f1();
    f2();
    f1();
    Console.WriteLine(total);
  end Main;
end Program;
