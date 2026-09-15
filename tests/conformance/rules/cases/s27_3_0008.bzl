//! case: s27_3_0008
//! doc: spec
//! section: 27.3
//! title: several ignore parameters may coexist in one lambda's parameter list
//! rule: A parameter slot may be the ignore `_`; because nothing is named,
//!   several ignores may coexist — `lambda(_, _) => 0` is legal.
//! cites: 27.3, 17.13
//! expect: run
//! stdout: 7
module Conf;

public class Program
  public static method Main()
    var pick: (int, int) -> int := lambda(_, _) => 7;
    Console.WriteLine($"{pick(1, 2)}");
  end Main;
end Program;
