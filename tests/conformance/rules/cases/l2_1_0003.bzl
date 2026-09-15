//! case: l2_1_0003
//! doc: stdlib
//! section: 2.1
//! title: Write does not append a line terminator, unlike WriteLine
//! rule: Console.Write(value) writes value with no terminator; consecutive
//!   Write calls land on the same output line, unlike WriteLine which
//!   terminates it.
//! expect: run
//! stdout: abcd
module Conf;

public class Program
  public static method Main()
    Console.Write("ab");
    Console.Write("cd");
    Console.WriteLine();
  end Main;
end Program;
