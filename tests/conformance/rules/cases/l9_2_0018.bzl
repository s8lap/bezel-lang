//! case: l9_2_0018
//! doc: stdlib
//! section: 9.2
//! title: Float.IeeeEquals rejects double arguments
//! rule: Float.IeeeEquals takes two float parameters; double to float is not in the
//!   implicit-conversion table of language spec 5.1, so a double argument is rejected.
//! cites: 5.1
//! expect: reject
//! diagnostic: convert
module Conf;

public class Program
  public static method Main()
    var d: double := 1.5;
    Console.WriteLine(Float.IeeeEquals(d, d));
  end Main;
end Program;
