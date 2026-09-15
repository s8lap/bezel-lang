//! case: s7_7_0004
//! doc: spec
//! section: 7.7
//! title: marking a named owned operand of ?? coalesces both arms into one owned result
//! rule: `owned ?? owned` is owned (the chosen value is discharged once); a
//!   named owned binding must be marked `move` to read as an owned arm.
//! cites: 28.3, 28.8.1
//! expect: run
//! stdout: chosen
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Choose(owned fallback: Widget?): owned Widget
    return move fallback ?? new Widget();
  end Choose;

  public static method Main()
    using var w := Choose(new Widget());
    Console.WriteLine("chosen");
  end Main;
end Program;
