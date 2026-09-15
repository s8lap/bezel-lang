//! case: l5_0010
//! doc: stdlib
//! section: 5
//! title: ToDictionary raises ArgumentException on a duplicate key
//! rule: ToDictionary (both overloads) raises ArgumentException on a
//!   duplicate key -- Dictionary.Add semantics; silent last-wins would
//!   mask a defective key selector.
//! cites: 5
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    using var xs: Array<int> := [1, 1, 2];
    try
      using var d := xs.ToDictionary(lambda(x: int): int => x);
    catch
      ArgumentException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
