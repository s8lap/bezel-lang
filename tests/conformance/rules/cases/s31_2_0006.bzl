//! case: s31_2_0006
//! doc: spec
//! section: 31.2
//! title: finally runs when the protected block exits by return
//! rule: `finally` runs whether the protected block completed normally, was
//!   exited by an exception, or was exited by `return`.
//! cites: 31.2
//! expect: run
//! stdout: finally
module Conf;

public class Program
  public static method Compute(): int
    try
      return 42;
    finally
      Console.WriteLine("finally");
    end try;
  end Compute;

  public static method Main()
    var r := Compute();
    Console.WriteLine(r);
  end Main;
end Program;
