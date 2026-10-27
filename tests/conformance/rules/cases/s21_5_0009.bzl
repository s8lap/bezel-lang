//! case: s21_5_0009
//! doc: spec
//! section: 21.5
//! title: a break out of a for-loop's try body runs that try's finally
//! rule: "On the way to its target, the jump exits every intervening scope —
//!   inner loop bodies, `do ... end` blocks, `try` bodies — and each performs
//!   its ordinary exit obligations" (§21.5, v0.340); a `finally` "runs whether
//!   the protected block completed normally, was exited by an exception
//!   (caught or uncaught), or was exited by `return` — or by a `break` or
//!   `continue` targeting an enclosing loop" (§31.2, v0.340). So a `break`
//!   out of a `for` body's `try` runs that `try`'s `finally`, skips the rest
//!   of the loop body, and continues after the loop.
//! cites: 21.5, 31.2
//! expect: run
//! output: fin 0
//!   body 0
//!   fin 1
//!   after loop
//!   outer fin
module Conf;

public class Program
  public static method Main(): int
    try
      for var i in 0..<3 do
        try
          if i = 1 then break; end;
        finally Console.WriteLine("fin " + i.ToString()); end;
        Console.WriteLine("body " + i.ToString());
      end for;
      Console.WriteLine("after loop");
    finally Console.WriteLine("outer fin"); end;
    return 5;
  end Main;
end Program;
