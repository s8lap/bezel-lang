//! case: s6_1_0002
//! doc: spec
//! section: 6.1
//! title: integer division truncates toward zero
//! rule: Division / between two integers truncates toward zero. var q := -7 /
//!   3; // -2.
//! cites: 6.1
//! expect: run
//! stdout: -2
module Conf;

public class Program
  public static method Main()
    var q := -7 / 3;
    Console.WriteLine(q);
  end Main;
end Program;
