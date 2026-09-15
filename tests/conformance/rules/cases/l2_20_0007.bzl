//! case: l2_20_0007
//! doc: stdlib
//! section: 2.20
//! title: UnwrapOr's fallback argument is evaluated eagerly
//! rule: UnwrapOr's argument is an ordinary value argument, not a deferred
//!   computation — it is evaluated at the call site, before the call, on
//!   the Ok path exactly as on the Error path.
//! cites: 2.20, 22.5
//! expect: run
//! stdout: computing fallback
module Conf;

public class Program
  public static method Fallback(): int
    Console.WriteLine("computing fallback");
    return -1;
  end Fallback;

  public static method Main()
    var r: Result<int, string> := Result<int, string>.Ok(42);
    var v := r.UnwrapOr(Fallback());
    Console.WriteLine(v);
  end Main;
end Program;
