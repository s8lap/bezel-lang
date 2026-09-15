//! case: s27_6_0001
//! doc: spec
//! section: 27.6
//! title: a static method reference is usable directly as a function value
//! rule: When a lambda's body is just a call to an existing method, the
//!   method reference may be used directly; method references work for static
//!   methods.
//! cites: 27.6
//! expect: run
//! stdout: 42
module Conf;

public class Program
  public static method Main()
    var parse: (string) -> int := int.Parse;
    Console.WriteLine($"{parse("42")}");
  end Main;
end Program;
