//! case: s22_5_0071
//! doc: spec
//! section: 22.5
//! title: the radix forms of Parse and TryParse evaluate their arguments left to right
//! rule: A call's **supplied** argument expressions are evaluated in **call-site textual order** — left to right as written
//! cites: 9.1
//! expect: run
//! output: F
//!   G
//!   True
//!   F
//!   G
//!   255
module Conf;
public class Program
  static method F(): string
    Console.WriteLine("F");
    return "ff";
  end F;

  static method G(): int
    Console.WriteLine("G");
    return 16;
  end G;

  public static method Main()
    var v: int := 0;
    Console.WriteLine(int.TryParse(F(), G(), out v));
    Console.WriteLine(int.Parse(F(), G()));
  end Main;
end Program;
