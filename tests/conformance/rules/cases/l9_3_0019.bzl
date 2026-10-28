//! case: l9_3_0019
//! doc: stdlib
//! section: 9.3
//! title: a value that rounds into range at scale 0 parses to MaxValue
//! rule: stdlib §9.3 (v0.345): "a value outside `decimal`'s range — one whose rounding does not
//!   fit at scale 0 … so `"79228162514264337593543950335.4"` parses to `MaxValue`".
//! cites: 9.3
//! expect: run
//! stdout: 79228162514264337593543950335
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(decimal.Parse("79228162514264337593543950335.4"));
  end Main;
end Program;
