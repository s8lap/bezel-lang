//! case: s17_14_0015
//! doc: spec
//! section: 17.14
//! title: an untaken if-expression branch contributes no constant-evaluation fault
//! rule: an operand that would not be evaluated at run time is not evaluated at compile time and contributes no fault
//! cites: 17.14, 20
//! expect: run
//! output: 3
//!   3
module Conf;

public class Program
  public static method Main()
    Console.WriteLine((if false then 2147483647 + 1 else 3));
    Console.WriteLine((if true then 3 else 2147483647 + 1));
  end Main;
end Program;
