//! case: s18_5_0005
//! doc: spec
//! section: 18.5
//! title: positive alignment right-aligns and negative alignment left-aligns in a field of the given width
//! rule: "Alignment is an integer, optionally negated; positive right-aligns in a
//!   field of that width, negative left-aligns." Example: "`$\"|{name,20}|\"`
//!   // right-aligned in 20-character field".
//! cites: 18.5
//! expect: run
//! stdout: |               Bezel|
module Conf;

public class Program
  public static method Main()
    var name := "Bezel";
    Console.WriteLine($"|{name,20}|");
    Console.WriteLine($"|{name,-20}|");
  end Main;
end Program;
