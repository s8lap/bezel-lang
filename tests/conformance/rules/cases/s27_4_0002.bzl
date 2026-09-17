//! case: s27_4_0002
//! doc: spec
//! section: 27.4
//! title: an explicit parameter type admits a top-level lambda with no target type
//! rule: In top-level contexts with no target type, parameter types must be
//!   specified; an explicitly typed parameter satisfies the requirement.
//! cites: 27.4
//! expect: run
//! stdout: 20
module Conf;

public class Program
  public static method Main()
    var f := lambda(x: int) => x * 2;
    Console.WriteLine($"{f(10)}");
  end Main;
end Program;
