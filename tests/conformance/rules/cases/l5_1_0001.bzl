//! case: l5_1_0001
//! doc: stdlib
//! section: 5.1
//! title: Sum() on an empty source returns the zero of its result type
//! rule: Sum without a selector is the constrained generic; on an empty
//!   source Sum (both forms) returns the zero of its result type.
//! cites: 5.1
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<int>();
    Console.WriteLine(xs.Sum());
  end Main;
end Program;
