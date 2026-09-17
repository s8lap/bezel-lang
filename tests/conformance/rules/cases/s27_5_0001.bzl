//! case: s27_5_0001
//! doc: spec
//! section: 27.5
//! title: a captured local is shared, not copied, between a closure and its enclosing scope
//! rule: A lambda may reference variables from its enclosing scope; captured
//!   variables are heap-allocated in a closure object, and modifications to a
//!   captured variable inside the lambda are visible outside, and vice versa.
//! cites: 27.5
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method MakeCounter(): () -> int
    var count := 0;
    return lambda() => do
      count := count + 1;
      return count;
    end;
  end MakeCounter;

  public static method Main()
    var counter := MakeCounter();
    counter();
    counter();
    Console.WriteLine($"{counter()}");
  end Main;
end Program;
