//! case: l9_6_0049
//! doc: stdlib
//! section: 9.6
//! title: string PadLeft and PadRight bind their named arguments in any written order
//! rule: PadLeft prepends and PadRight appends paddingChar until the result's ... public pure method PadLeft(totalWidth: int, paddingChar: char := ' '): string;
//! cites: 9.6, 22.5
//! expect: run
//! output: *********hello world
//!   hello world****|
module Conf;

public class Program
  public static method Main()
    var s := "hello world";
    Console.WriteLine(s.PadLeft(paddingChar := '*', totalWidth := 20));
    Console.WriteLine(s.PadRight(paddingChar := '*', totalWidth := 15) + "|");
  end Main;
end Program;
