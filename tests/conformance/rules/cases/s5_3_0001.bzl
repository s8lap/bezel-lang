//! case: s5_3_0001
//! doc: spec
//! section: 5.3
//! title: a nullable-to-nullable cast maps a null operand to null
//! rule: A null operand yields null; var y: long? := long?(x); // nullable to
//!   nullable; null stays null.
//! cites: 5.3
//! expect: run
//! stdout: [null]
module Conf;

public class Program
  public static method Main()
    var x: int? := null;
    var y := long?(x);
    Console.WriteLine($"[{y}]");
  end Main;
end Program;
