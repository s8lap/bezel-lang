//! case: s22_5_0020
//! doc: spec
//! section: 22.5
//! title: a named argument on a string member binds to the parameter it names
//! rule: stdlib §9.6 declares `Remove(startIndex: int, length: int): string` and
//!   `Repeat(c: char, count: int): string`. §22.5: named arguments may appear in
//!   any order after positional arguments, and each supplied argument binds
//!   exactly one parameter - so `Remove(length := 2, startIndex := 1)` removes
//!   two characters starting at index 1, and `Repeat(count := 3, c := 'x')`
//!   repeats "x" three times.
//! cites: 22.5, 9.6
//! expect: run
//! output: adef
//!   xxx
//! issue: 7199
module Conf;

public class Program
  public static method Main()
    var s := "abcdef";
    Console.WriteLine(s.Remove(length := 2, startIndex := 1));
    Console.WriteLine(string.Repeat(count := 3, c := 'x'));
  end Main;
end Program;
