//! case: s31_2_0005
//! doc: spec
//! section: 31.2
//! title: finally runs when the protected block completes normally
//! rule: `finally` runs whether the protected block completed normally, was
//!   exited by an exception (caught or uncaught), or was exited by `return`.
//! cites: 31.2
//! expect: run
//! stdout: finally
module Conf;

public class Program
  public static method Main()
    try
      var x := 1 + 1;
    finally
      Console.WriteLine("finally");
    end try;
  end Main;
end Program;
