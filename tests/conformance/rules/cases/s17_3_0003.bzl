//! case: s17_3_0003
//! doc: spec
//! section: 17.3
//! title: inline-array equality uses default structural equality and cannot be overloaded
//! rule: "For inline arrays, `=` cannot be overloaded (inline arrays are
//!   neither classes, records, nor enums, and the §26.2 extension rules
//!   forbid extending them); the default structural equality of §8.3 is
//!   used."
//! cites: 17.3, 8.3, 26.2
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var a: [3]int := [1, 2, 3];
    var b: [3]int := [1, 2, 3];
    var eq := a = b;
    Console.WriteLine(eq);
  end Main;
end Program;
