//! case: s6_6_0010
//! doc: spec
//! section: 6.6
//! title: a float-typed result against a float target stores directly
//! rule: Let T be the left-hand side's type and R the type of x op e; if R is
//!   T, or R implicitly converts to T, the result is stored directly — write
//!   f += 2.5f; float promotion stays in-family.
//! cites: 6.6, 6.2
//! expect: run
//! stdout: 3.5
module Conf;

public class Program
  public static method Main()
    var f: float := 1.0f;
    f += 2.5f;
    Console.WriteLine(f);
  end Main;
end Program;
