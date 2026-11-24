//! case: s28_3_1_7830
//! doc: spec
//! section: 28.3.1
//! title: a local carried past a ?? throw operand stays owned and readable on the continuing path
//! rule: `null`-literal and bottom-typed (`throw`) arms absorb to the other arm's
//!   state
//! cites: 17.12, 7.7, 28.3.1
//! expect: run
//! output: b
//!   5
//!   7
module Conf;
import System;

public class Program
  public static method Run(k: int?): int
    try
      var e := new InvalidOperationException("b");
      var t := k ?? throw e;
      Console.WriteLine(e.Message);
      return t;
    catch
      InvalidOperationException => do return 7; end
    end;
  end Run;
  public static method Main(): int
    Console.WriteLine(Run(5));
    Console.WriteLine(Run(null));
    return 0;
  end Main;
end Program;
