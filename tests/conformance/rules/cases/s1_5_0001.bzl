//! case: s1_5_0001
//! doc: spec
//! section: 1.5
//! title: block comments do not nest
//! rule: `/* ... */` — block comment, does not nest.
//! expect: run
//! stdout: reached
module Conf;

public class Program
  public static method Main()
    /* first /* second */ Console.WriteLine("reached"); /* a plain comment */
    Console.WriteLine("done");
  end Main;
end Program;
