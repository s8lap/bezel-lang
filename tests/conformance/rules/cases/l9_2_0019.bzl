//! case: l9_2_0019
//! doc: stdlib
//! section: 9.2
//! title: Float.IsFinite rejects a double literal argument
//! rule: Float.IsFinite takes a float parameter; double to float is not an implicit
//!   conversion (language spec 5.1), and 3.0 is a double literal.
//! cites: 5.1
//! expect: reject
//! diagnostic: convert
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Float.IsFinite(3.0));
  end Main;
end Program;
