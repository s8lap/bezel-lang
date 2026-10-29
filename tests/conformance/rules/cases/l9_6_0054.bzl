//! case: l9_6_0054
//! doc: stdlib
//! section: 9.6
//! title: string Substring, Remove and AsSpan bind their named window arguments in reverse written order
//! rule: public pure method Substring(startIndex: int, length: int): string; ... public pure method Remove(startIndex: int, length: int): string; ... public pure method AsSpan(start: int, length: int): ReadOnlyStringSpan;
//! cites: 9.6, 22.5
//! expect: run
//! output: llo
//!   he world
//!   2
module Conf;

public class Program
  public static method Main()
    var s := "hello world";
    Console.WriteLine(s.Substring(length := 3, startIndex := 2));
    Console.WriteLine(s.Remove(length := 3, startIndex := 2));
    Console.WriteLine(s.AsSpan(length := 2, start := 1).Length);
  end Main;
end Program;
