//! case: s18_5_0004
//! doc: spec
//! section: 18.5
//! title: the X format spec renders an integer in uppercase hex
//! rule: "`$\"{value:X}\"`            // \"FF\""
//! cites: 18.5
//! expect: run
//! stdout: FF
module Conf;

public class Program
  public static method Main()
    var value: int := 255;
    Console.WriteLine($"{value:X}");
  end Main;
end Program;
