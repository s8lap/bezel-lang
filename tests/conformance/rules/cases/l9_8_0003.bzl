//! case: l9_8_0003
//! doc: stdlib
//! section: 9.8
//! title: IsEmpty is true for an empty exclusive-bound range
//! rule: public pure readonly property IsEmpty: bool; // true if the
//!   range contains no values
//! cites: 9.8
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine((5..<5).IsEmpty);
  end Main;
end Program;
