//! case: l2_1_9951
//! doc: stdlib
//! section: 2.1
//! title: Console.Write of Console.In.ReadLine() defaulted with ?? is accepted
//! rule: has no applicable candidate and is a compile error
//! cites: 2.1, 7.4
//! expect: run
//! stdout: none
//! notes: Twin of l2_1_9950: the only edit is the `?? "none"`, which makes the argument a string. Empty
//!   stdin, so ReadLine() is null and the default prints.
module Conf;
public class Program
  public static method Main()
    Console.Write(Console.In.ReadLine() ?? "none");
  end Main;
end Program;
