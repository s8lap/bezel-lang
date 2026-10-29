//! case: l9_6_0061
//! doc: stdlib
//! section: 9.6
//! title: string LastIndexOf(string): last start index, -1 when absent, Length for the empty string
//! rule: -1 when absent; LastIndexOf("") = Length
//! cites: 9.6
//! expect: run
//! output: 4
//!   -1
//!   6
module Conf;

public class Program
  public static method Main()
    var s: string := "abcabc";
    Console.WriteLine(s.LastIndexOf("bc"));
    Console.WriteLine(s.LastIndexOf("z"));
    Console.WriteLine(s.LastIndexOf(""));
  end Main;
end Program;
