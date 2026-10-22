//! case: s17_14_0016
//! doc: spec
//! section: 17.14
//! title: a short-circuited boolean operand contributes no constant-evaluation fault
//! rule: the boolean operators' short-circuiting (an operand that would not be evaluated at run time is not evaluated at compile time and contributes no fault).
//! cites: 17.14, 17.2
//! expect: run
//! output: True
//!   False
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(true or 2147483647 + 1 > 0);
    Console.WriteLine(false and 2147483647 + 1 > 0);
  end Main;
end Program;
