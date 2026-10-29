//! case: l9_6_0062
//! doc: stdlib
//! section: 9.6
//! title: string LastIndexOf(char) gives the index of the last occurrence
//! rule: public pure method LastIndexOf(value: char): int;   // last occurrence, or -1
//! cites: 9.6
//! expect: run
//! output: 5
//!   0
//!   -1
module Conf;

public class Program
  public static method Main()
    var s: string := "abcabc";
    Console.WriteLine(s.LastIndexOf('c'));
    Console.WriteLine(s.LastIndexOf('a') - 3);
    Console.WriteLine(s.LastIndexOf('z'));
  end Main;
end Program;
