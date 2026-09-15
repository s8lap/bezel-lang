//! case: s4_2_0001
//! doc: spec
//! section: 4.2
//! title: byte.MaxValue is 255
//! rule: byte | 8, unsigned | 0 to 255.
//! cites: 4.2
//! expect: run
//! stdout: 255
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{byte.MaxValue}");
  end Main;
end Program;
