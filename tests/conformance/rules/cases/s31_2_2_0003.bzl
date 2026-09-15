//! case: s31_2_2_0003
//! doc: spec
//! section: 31.2.2
//! title: a do...end block catch arm body holds several statements
//! rule: A catch arm's body may be a `do ... end` block: `=> do statements
//!   end`, used when the body contains multiple statements.
//! cites: 31.2.2
//! expect: run
//! output: step1
//!   step2
module Conf;

public class BoomException: Exception
  public constructor(message: string) base(message)
  end constructor;
end BoomException;

public class Program
  public static method Main()
    try
      throw new BoomException("x");
    catch
      var ex: BoomException => do
        Console.WriteLine("step1");
        Console.WriteLine("step2");
      end,
    end try;
  end Main;
end Program;
