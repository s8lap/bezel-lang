//! case: s28_10_0017
//! doc: spec
//! section: 28.10
//! title: a using var of the protected block is disposed before that try's finally on the loop-exit edge
//! rule: "A `try` statement's own `finally` is established at entry to the
//!   protected block — before any establishment the block itself performs.
//!   Consequently, on every exit edge, an owned local or `using var` declared
//!   inside the protected block (established later) is disposed before the
//!   `finally` runs ...; cleanups established before the `try` run after the
//!   `finally`, as before" (§28.10, v0.340). So a `using var` declared
//!   inside the protected block is still alive after a `break` out of a
//!   nested `try` — the `break` leaves only that inner `try` — and it is
//!   disposed before the enclosing `finally` runs.
//! cites: 28.10, 31.2
//! expect: run
//! output: fin 1
//!   fin 2
//!   after loop r=1
//!   dispose 1
//!   outer
module Conf;

public class Res
  public N: int;
  public constructor(n: int) N := n; end constructor;
  destructor Console.WriteLine("dispose " + N.ToString()); end destructor;
end Res;

public class Program
  public static method Main(): int
    try
      using var r := new Res(1);
      var i := 0;
      while i < 3 do
        i := i + 1;
        try
          if i = 2 then break; end;
        finally Console.WriteLine("fin " + i.ToString()); end try;
      end while;
      Console.WriteLine("after loop r=" + r.N.ToString());
    finally Console.WriteLine("outer"); end try;
    return 7;
  end Main;
end Program;
