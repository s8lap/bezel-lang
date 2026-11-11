//! case: l2_3_0002
//! doc: stdlib
//! section: 2.3
//! title: ToInt32 raises an OverflowException when the value does not fit
//! rule: The `To*` methods raise `FormatException` when the string is not a
//!   well-formed representation of the target type and `OverflowException` when
//!   the value does not fit — the same contract as the §9 per-type `Parse`
//!   family these conversions mirror
//! expect: run
//! stdout: threw
module Conf;

public class Program
  public static method Main()
    try
      _ := Convert.ToInt32("99999999999");
      Console.WriteLine("no exception");
    catch
      OverflowException => Console.WriteLine("threw")
    end try;
  end Main;
end Program;
