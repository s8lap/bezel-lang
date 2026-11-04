//! case: s18_5_0020
//! doc: spec
//! section: 18.5
//! title: alignment applies to a present nullable function hole
//! rule: Alignment is an integer, optionally negated; positive right-aligns in a field of that
//!   width, negative left-aligns.
//! cites: 18.5, 8.5, 27.1
//! expect: run
//! stdout: [  (int) -> int]
module Conf;

public class Program
  public static method Main()
    var f1: ((int) -> int)? := lambda(x: int) => x;
    Console.WriteLine($"[{f1,14}]");
  end Main;
end Program;
