//! case: l9_6_0041
//! doc: stdlib
//! section: 9.6
//! title: named arguments unit and count bind the string overload of Repeat
//! rule: stdlib §9.6 declares `public static pure method Repeat(unit: string, count: int): string;`.
//!   Language spec §22.5: each supplied argument binds exactly one parameter, so `unit := "ab"` binds
//!   the parameter named unit and `count := 0` binds the parameter named count.
//! cites: 9.6, 22.5
//! expect: run
//! stdout: 0
//! issue: 7129
//! notes: the named form selects the unit overload; the char overload has no parameter named unit.
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(string.Repeat(unit := "ab", count := 0).Length);
  end Main;
end Program;
