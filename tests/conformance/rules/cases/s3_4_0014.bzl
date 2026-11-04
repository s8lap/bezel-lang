//! case: s3_4_0014
//! doc: spec
//! section: 3.4
//! title: String.From over a char buffer window equals string.From
//! rule: public static pure method From(buffer: Array<char>, start: int, length:
//!   int): string;
//! cites: 3.4, 9.6
//! expect: run
//! output: yz
//!   yz
//! issue: 7494
//! notes: twin of s3_4_0015 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Main()
    using var cs := new Array<char>();
    cs.Add('x');
    cs.Add('y');
    cs.Add('z');
    Console.WriteLine(String.From(cs, 1, 2));
    Console.WriteLine(string.From(cs, 1, 2));
  end Main;
end Program;
