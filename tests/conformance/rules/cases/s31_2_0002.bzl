//! case: s31_2_0002
//! doc: spec
//! section: 31.2
//! title: a try statement with only a finally is admitted
//! rule: If the `try` has only a `finally` (no `catch`), the catch block is
//!   omitted entirely, satisfying the at-least-one-of-catch-or-finally rule.
//! cites: 31.2
//! expect: run
//! stdout: working
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine("working");
    finally
      Console.WriteLine("finally");
    end try;
  end Main;
end Program;
