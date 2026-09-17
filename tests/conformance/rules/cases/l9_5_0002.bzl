//! case: l9_5_0002
//! doc: stdlib
//! section: 9.5
//! title: char.IsDigit classifies a decimal digit
//! rule: static pure method IsDigit(c: char): bool;
//! cites: 9.5
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(char.IsDigit('7'));
  end Main;
end Program;
