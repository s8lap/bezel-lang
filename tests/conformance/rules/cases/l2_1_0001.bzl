//! case: l2_1_0001
//! doc: stdlib
//! section: 2.1
//! title: ReadLine returns null at end of input
//! rule: Console.ReadLine() returns null at end of input.
//! cites: 1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Console.ReadLine() = null);
  end Main;
end Program;
