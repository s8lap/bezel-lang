//! case: s21_5_0007
//! doc: spec
//! section: 21.5
//! title: a continue out of a try body runs that try's finally and skips the rest of the body
//! rule: "On the way to its target, the jump exits every intervening scope —
//!   inner loop bodies, `do ... end` blocks, `try` bodies — and each performs
//!   its ordinary exit obligations" (§21.5, v0.340); a `finally` "runs whether
//!   the protected block completed normally, was exited by an exception
//!   (caught or uncaught), or was exited by `return` — or by a `break` or
//!   `continue` targeting an enclosing loop" (§31.2, v0.340). So a
//!   `continue` out of a `try` body runs that `try`'s `finally`, skips the
//!   statements after the `try` in the loop body, and the next iteration
//!   starts.
//! cites: 21.5, 31.2
//! expect: run
//! output: fin 1
//!   body 1
//!   fin 2
//!   fin 3
//!   body 3
//!   fin 4
//!   body 4
//!   after loop 4
//!   outer
module Conf;

public class Program
  public static method Main(): int
    try
      var i := 0;
      while i < 4 do
        i := i + 1;
        try
          if i = 2 then continue; end;
        finally
          Console.WriteLine("fin " + i.ToString());
        end try;
        Console.WriteLine("body " + i.ToString());
      end while;
      Console.WriteLine("after loop " + i.ToString());
    finally
      Console.WriteLine("outer");
    end try;
    return 7;
  end Main;
end Program;
