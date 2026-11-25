//! case: l7_4_9401
//! doc: stdlib
//! section: 7.4
//! title: the TextWriter members Write and WriteLine stay admitted on Console.Out
//! rule: `TextWriter`'s `WriteLine` members write the argument followed by `Environment.NewLine`
//! cites: 2.1, 8.5
//! expect: run
//! output: ac42True1.5
//!   s
//!   7
module Conf;

public class Program
  public static method Main()
    Console.Out.Write("a");
    Console.Out.Write('c');
    Console.Out.Write(42);
    Console.Out.Write(true);
    Console.Out.Write(1.5);
    Console.Out.WriteLine();
    Console.Out.WriteLine("s");
    Console.Out.WriteLine(7);
    Console.Out.Flush();
  end Main;
end Program;
