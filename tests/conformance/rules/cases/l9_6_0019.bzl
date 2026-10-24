//! case: l9_6_0019
//! doc: stdlib
//! section: 9.6
//! title: a call to Substring supplying two arguments binds the two-argument form
//! rule: public pure method Substring(startIndex: int, length: int): string;
//!   — the call supplies exactly the declared form's arguments, so the member
//!   is applicable and the call is admitted.
//! cites: 9.6, 22.5, 22.6
//! expect: run
//! stdout: ab
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("abcdef".Substring(0, 2));
  end Main;
end Program;
