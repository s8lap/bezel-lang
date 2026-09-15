//! case: s20_0012
//! doc: spec
//! section: 20
//! title: elif conditions are evaluated in textual order and only up to the first true one
//! rule: "**Evaluation.** Conditions are evaluated in textual order; the first
//!   `true` condition selects its branch ... and only the selected branch is
//!   evaluated." The second condition is true, so the third is never evaluated.
//! cites: 20
//! expect: run
//! stdout: AB
module Conf;

public class Program
  private static _hits: string := "";

  public static method CheckA(): bool
    _hits := _hits + "A";
    return false;
  end CheckA;

  public static method CheckB(): bool
    _hits := _hits + "B";
    return true;
  end CheckB;

  public static method CheckC(): bool
    _hits := _hits + "C";
    return true;
  end CheckC;

  public static method Main()
    var v := (if CheckA() then 1 elif CheckB() then 2 elif CheckC() then 3 else 4);
    Console.WriteLine(_hits);
    Console.WriteLine(v);
  end Main;
end Program;
