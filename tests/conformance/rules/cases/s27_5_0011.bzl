//! case: s27_5_0011
//! doc: spec
//! section: 27.5
//! title: a local captured two lambda levels down is shared with the enclosing scope
//! rule: A lambda may reference variables from its enclosing scope. Captured
//!   variables are heap-allocated in a closure object. Modifications to a
//!   captured variable inside the lambda are visible outside, and vice versa.
//!   (v0.338 §27.5)
//! cites: 27.5
//! expect: run
//! output: 2
module Conf;

public class Program
  public static method Main()
    var count := 0;
    var make := lambda() => lambda() => do
      count := count + 1;
    end;
    var bump := make();
    bump();
    bump();
    Console.WriteLine(count);
  end Main;
end Program;
