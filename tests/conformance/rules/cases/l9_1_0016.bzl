//! case: l9_1_0016
//! doc: stdlib
//! section: 9.1
//! title: TryParse(s, radix, out) raises ArgumentException on an invalid radix
//! rule: stdlib §9.1 declares `TryParse(s: string, radix: int, out result: int):
//!     bool` with the note "an invalid radix raises
//!     ArgumentException from both forms" — the radix set is {2, 8, 10, 16}.
//! cites: 9.1
//! expect: run
//! stdout: caught
//! issue: 7268
module Conf;

public class Program
  public static method Main()
    try
      _ := int.TryParse("10", 3, out var v);
    catch
      ArgumentException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
