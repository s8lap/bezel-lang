//! case: l2_18_0003
//! doc: stdlib
//! section: 2.18
//! title: integer B renders binary digits
//! rule: Integers admit B binary.
//! cites: 2.18
//! expect: run
//! stdout: 101
module Conf;

public class Program
  public static method Main()
    var n := 5;
    Console.WriteLine($"{n:B}");
  end Main;
end Program;
