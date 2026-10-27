//! case: s21_5_0008
//! doc: spec
//! section: 21.5
//! title: a labeled break out of a nested loop runs the finally of the try it exits
//! rule: "`break name;` exits the enclosing loop labeled `name`. After
//!   `break name;`, execution continues at the first statement after the
//!   labeled loop" (§21.5, v0.340); on the way to that target the jump
//!   "exits every intervening scope — inner loop bodies, `do ... end` blocks,
//!   `try` bodies — and each performs its ordinary exit obligations", and a
//!   `finally` "runs whether the protected block completed normally, was
//!   exited by an exception (caught or uncaught), or was exited by `return` —
//!   or by a `break` or `continue` targeting an enclosing loop" (§31.2,
//!   v0.340). So the labeled `break` runs the `try`'s `finally`, skips the
//!   rest of the inner loop body, and continues after the labeled loop.
//! cites: 21.5, 31.2
//! expect: run
//! output: body
//!   fin
//!   after outer
//!   outer fin
module Conf;

public class Program
  public static method Main(): int
    try
      outer: while true do
        while true do
          try
            Console.WriteLine("body");
            break outer;
          finally Console.WriteLine("fin"); end;
        end;
        Console.WriteLine("after inner");
      end outer;
      Console.WriteLine("after outer");
    finally Console.WriteLine("outer fin"); end;
    return 5;
  end Main;
end Program;
