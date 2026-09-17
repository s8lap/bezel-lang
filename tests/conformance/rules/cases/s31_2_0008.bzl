//! case: s31_2_0008
//! doc: spec
//! section: 31.2
//! title: break inside a finally whose target loop encloses it is rejected
//! rule: Neither may exit a `finally` block: inside a `finally`, a `break` or
//!   `continue` — labeled or unlabeled — whose target loop lies outside the
//!   `finally` is a compile error, the same rule as `return` in a `finally`.
//! cites: 31.2, 21.5
//! expect: reject
//! diagnostic: break
module Conf;

public class Program
  public static method Main()
    scan: while true do
      try
        Console.WriteLine("try");
      finally
        break scan;
      end try;
    end scan;
  end Main;
end Program;
