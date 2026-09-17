//! case: s5_2_0009
//! doc: spec
//! section: 5.2
//! title: casting an integer in the code-point range to char yields that code point
//! rule: Casting an integer to char requires the integer to fall in
//!   U+0000–U+10FFFF excluding the surrogate range; var c: char := char(n); //
//!   n must be a valid code point.
//! cites: 5.2
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var n: int := 65;
    var c := char(n);
    Console.WriteLine(c = 'A');
  end Main;
end Program;
