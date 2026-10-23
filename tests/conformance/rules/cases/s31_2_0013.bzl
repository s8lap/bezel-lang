//! case: s31_2_0013
//! doc: spec
//! section: 31.2
//! title: a return out of a try body runs the inner finally and then the outer finally
//! rule: A `finally` "runs whether the protected block completed normally,
//!   was exited by an exception (caught or uncaught), or was exited by
//!   `return` — or by a `break` or `continue` targeting an enclosing loop"
//!   (§31.2, v0.340), and the cleanups run in reverse establishment order.
//!   So a `return` out of the inner `try` body runs the inner `finally` and
//!   then the outer `finally`, and the statements after the inner `try` —
//!   and after the outer `try` — are skipped.
//! cites: 31.2, 21.5
//! expect: run
//! output: fin 1
//!   fin 2
//!   outer fin
module Conf;

public class Program
  public static method Main(): int
    try
      var i := 0;
      while i < 3 do
        i := i + 1;
        try
          if i = 2 then return 9; end;
        finally Console.WriteLine("fin " + i.ToString()); end;
      end while;
      Console.WriteLine("after loop");
    finally Console.WriteLine("outer fin"); end;
    return 5;
  end Main;
end Program;
