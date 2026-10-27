//! case: s21_5_0011
//! doc: spec
//! section: 21.5
//! title: a break that exits only the inner loop does not run the enclosing try's finally
//! rule: On the way to its target the jump "exits every intervening scope —
//!   inner loop bodies, `do ... end` blocks, `try` bodies — and each performs
//!   its ordinary exit obligations" (§21.5, v0.340); a `finally` "runs whether
//!   the protected block completed normally, was exited by an exception
//!   (caught or uncaught), or was exited by `return` — or by a `break` or
//!   `continue` targeting an enclosing loop" (§31.2, v0.340). The `try` whose
//!   `finally` runs is the one the jump passes through: a `break` out of the
//!   inner loop runs the inner `try`'s `finally` and leaves the `try` that
//!   encloses the target loop untouched, so that `finally` runs only when its
//!   own protected block ends.
//! cites: 21.5, 31.2
//! expect: run
//! output: inner body
//!   inner fin
//!   after inner while 1
//!   inner body
//!   inner fin
//!   after inner while 2
//!   after outer while
//!   outer fin
module Conf;

public class Program
  public static method Main(): int
    try
      var j := 0;
      while j < 2 do
        j := j + 1;
        while true do
          try
            Console.WriteLine("inner body");
            break;
          finally Console.WriteLine("inner fin"); end;
        end;
        Console.WriteLine("after inner while " + j.ToString());
      end;
      Console.WriteLine("after outer while");
    finally Console.WriteLine("outer fin"); end;
    return 5;
  end Main;
end Program;
