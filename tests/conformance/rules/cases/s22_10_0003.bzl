//! case: s22_10_0003
//! doc: spec
//! section: 22.10
//! title: a primary constructor parameter referenced in a body position is captured and readable from methods
//! rule: The parameters are in scope inside the body of any method,
//!   property, constructor, or destructor of the class. The compiler
//!   captures them as private fields automatically.
//! cites: 22.10
//! expect: run
//! stdout: 5
module Conf;

public class Counter(start: int)
  public method Value(): int => start;
end Counter;

public class Program
  public static method Main()
    var c := new Counter(5);
    Console.WriteLine(c.Value());
  end Main;
end Program;
