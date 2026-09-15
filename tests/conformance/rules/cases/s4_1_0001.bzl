//! case: s4_1_0001
//! doc: spec
//! section: 4.1
//! title: bool renders as True or False
//! rule: bool has two values, true and false.
//! cites: 4.1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{true}");
  end Main;
end Program;
