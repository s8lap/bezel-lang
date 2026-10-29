//! case: l9_6_0040
//! doc: stdlib
//! section: 9.6
//! title: string.Repeat over a non-empty unit with a negative count raises ArgumentOutOfRangeException
//! rule: public static pure method Repeat(unit: string, count: int): string; // same count contract;
//!   the char overload's contract states count < 0 raises ArgumentOutOfRangeException
//! cites: 9.6
//! expect: run
//! stdout: caught
//! issue: 7129
//! notes: the unit overload with a non-empty unit. l9_6_0025 pins the char overload.
module Conf;

public class Program
  public static method Main()
    try
      _ := string.Repeat("ab", -1);
    catch
      var ex: ArgumentOutOfRangeException => Console.WriteLine("caught");
    end try;
  end Main;
end Program;
