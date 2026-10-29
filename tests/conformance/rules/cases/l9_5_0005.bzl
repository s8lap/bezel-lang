//! case: l9_5_0005
//! doc: stdlib
//! section: 9.5
//! title: char.ToUpper accepts its parameter by name
//! rule: char.ToUpper is declared ToUpper(c: char): char; a named argument binds to
//!   the declared parameter name (language spec 22.5).
//! cites: 22.5
//! expect: run
//! output: A
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(char.ToUpper(c := 'a'));
  end Main;
end Program;
