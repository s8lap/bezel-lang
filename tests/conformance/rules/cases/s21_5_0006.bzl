//! case: s21_5_0006
//! doc: spec
//! section: 21.5
//! title: a break out of a try body runs that try's finally on the jump edge
//! rule: "On the way to its target, the jump exits every intervening scope —
//!   inner loop bodies, `do ... end` blocks, `try` bodies — and each performs
//!   its ordinary exit obligations" (§21.5, v0.340); a `finally` "runs whether
//!   the protected block completed normally, was exited by an exception
//!   (caught or uncaught), or was exited by `return` — or by a `break` or
//!   `continue` targeting an enclosing loop" (§31.2, v0.340). So a `break`
//!   out of a `try` body runs that `try`'s `finally` on the way out, the
//!   statements after the `try` in the loop body are skipped, and the
//!   statement after the loop runs next.
//! cites: 21.5, 31.2
//! expect: run
//! output: fin 1
//!   body 1
//!   fin 2
//!   after loop 2
//!   outer
module Conf;

public class Program
  public static method Main(): int
    try
      var i := 0;
      while i < 3 do
        i := i + 1;
        try
          if i = 2 then break; end;
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
