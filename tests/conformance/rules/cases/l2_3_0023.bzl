//! case: l2_3_0023
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToDouble raises FormatException for the spellings the double input grammar forbids
//! rule: The `To*` methods raise `FormatException` when the string is not a
//!   well-formed representation of the target type and `OverflowException` when
//!   the value does not fit — the same contract as the §9 per-type `Parse`
//!   family these conversions mirror
//! cites: 2.3, 9.2
//! expect: run
//! issue: 7324
//! output: [+.5] -> FMT
//!   [.5] -> FMT

module Conf;

public class Program
  public static method Main()
    Show("+.5");
    Show(".5");
  end Main;

  private static method Show(s: string)
    try
      _ := Convert.ToDouble(s);
      Console.WriteLine("[" + s + "] -> OK");
    catch
      FormatException => Console.WriteLine("[" + s + "] -> FMT");
    end;
  end Show;
end Program;
