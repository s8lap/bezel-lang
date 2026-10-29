//! case: l9_6_0050
//! doc: stdlib
//! section: 9.6
//! title: string IndexOf binds named startIndex before value for both the string and the char overload
//! rule: public pure method IndexOf(value: char, startIndex: int := 0): int; ... public pure method IndexOf(value: string, startIndex: int := 0): int;
//! cites: 9.6, 22.5
//! expect: run
//! output: 9
//!   7
module Conf;

public class Program
  public static method Main()
    var s := "hello world";
    Console.WriteLine(s.IndexOf(startIndex := 4, value := "l"));
    Console.WriteLine(s.IndexOf(startIndex := 5, value := 'o'));
  end Main;
end Program;
