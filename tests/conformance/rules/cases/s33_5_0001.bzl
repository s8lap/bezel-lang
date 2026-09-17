//! case: s33_5_0001
//! doc: spec
//! section: 33.5
//! title: a class declaring two qualifying Main overloads is rejected
//! rule: A program is ill-formed if more than one method qualifies as the
//!   entry point ... within a single class (one class declaring more than
//!   one of the eight signature shapes). A class that overloads `Main` with
//!   multiple qualifying signatures is rejected at compile time.
//! cites: 33.1, C.12
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("a");
  end Main;

  public static method Main(args: Array<string>)
    Console.WriteLine("b");
  end Main;
end Program;
