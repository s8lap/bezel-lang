//! case: s22_5_4601
//! doc: spec
//! section: 22.5
//! title: a reordered named call of int.Parse binds each argument to the parameter it names
//! rule: Named arguments may appear in any order after positional arguments.
//! cites: 9.1
//! expect: run
//! output: 255
//!   255
//!   12
//! issue: 7602
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(int.Parse(radix := 16, s := "ff"));
    Console.WriteLine(int.Parse("ff", radix := 16));
    Console.WriteLine(int.Parse(s := "12"));
  end Main;
end Program;
