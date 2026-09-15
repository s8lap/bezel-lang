//! case: s31_2_0009
//! doc: spec
//! section: 31.2
//! title: continue inside a finally whose target loop encloses it is rejected
//! rule: Neither may exit a `finally` block: inside a `finally`, a `break` or
//!   `continue` whose target loop lies outside the `finally` is a compile
//!   error — on the exception edge such a jump would silently discard the
//!   in-flight exception.
//! cites: 31.2, 21.5
//! expect: reject
//! diagnostic: continue
module Conf;

public class Program
  public static method Main()
    scan: for var i in 1..3 do
      try
        Console.WriteLine("try");
      finally
        continue scan;
      end try;
    end scan;
  end Main;
end Program;
