//! case: l9_3_0008
//! doc: stdlib
//! section: 9.3
//! title: decimal.MinValue is the exact negated 29-digit constant
//! rule: stdlib §9.3 (v0.345) "Rendering and range": "`MinValue` is `-79228162514264337593543950335`".
//! cites: 9.3
//! expect: run
//! stdout: -79228162514264337593543950335
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(decimal.MinValue);
  end Main;
end Program;
