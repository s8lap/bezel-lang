//! case: s21_5_0010
//! doc: spec
//! section: 21.5
//! title: two finallys run in reverse establishment order on a break edge
//! rule: On the way to its target the jump "exits every intervening scope —
//!   inner loop bodies, `do ... end` blocks, `try` bodies — and each performs
//!   its ordinary exit obligations" (§21.5, v0.340); a `finally` "runs whether
//!   the protected block completed normally, was exited by an exception
//!   (caught or uncaught), or was exited by `return` — or by a `break` or
//!   `continue` targeting an enclosing loop" (§31.2, v0.340), and the
//!   cleanups run in reverse establishment order. So a `break` out of the
//!   inner `try` body runs the inner `finally` and then the outer `finally`,
//!   the inner one first — it was established last.
//! cites: 21.5, 31.2
//! expect: run
//! output: inner body
//!   inner fin
//!   outer fin
//!   after inner while 1
//!   inner body
//!   inner fin
//!   outer fin
//!   after inner while 2
//!   done
module Conf;

public class Program
  public static method Main(): int
    var j := 0;
    while j < 2 do
      j := j + 1;
      while true do
        try
          try
            Console.WriteLine("inner body");
            break;
          finally Console.WriteLine("inner fin"); end;
        finally Console.WriteLine("outer fin"); end;
      end;
      Console.WriteLine("after inner while " + j.ToString());
    end;
    Console.WriteLine("done");
    return 5;
  end Main;
end Program;
