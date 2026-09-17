//! case: s4_5_0001
//! doc: spec
//! section: 4.5
//! title: char values compare by code-point value
//! rule: Character values compare by code-point value: 'A' < 'B'.
//! cites: 4.5
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{'A' < 'B'}");
  end Main;
end Program;
