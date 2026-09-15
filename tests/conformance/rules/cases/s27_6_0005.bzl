//! case: s27_6_0005
//! doc: spec
//! section: 27.6
//! title: a type annotation disambiguates a reference to an overloaded method
//! rule: The user disambiguates an ambiguous method reference by adding a type
//!   annotation: `var f: (int) -> int := Math.Abs;`.
//! cites: 27.6
//! expect: run
//! stdout: 7
module Conf;

public class Program
  public static method Main()
    var f: (int) -> int := Math.Abs;
    Console.WriteLine($"{f(-7)}");
  end Main;
end Program;
