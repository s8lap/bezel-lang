//! case: s18_5_0013
//! doc: spec
//! section: 18.5
//! title: an unescaped closing brace that does not close a hole is a compile error
//! rule: "An unescaped `}` that does not close a hole is a compile error; write
//!   `}}` for a literal closing brace."
//! cites: 18.5
//! expect: reject
//! diagnostic: does not close
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"abc}def");
  end Main;
end Program;
