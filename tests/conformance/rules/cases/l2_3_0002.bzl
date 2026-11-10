//! case: l2_3_0002
//! doc: stdlib
//! section: 2.3
//! title: ToInt32 raises an OverflowException (ParseRangeException) when the value does not fit
//! rule: The `To*` methods raise `FormatException` when the string is not a
//!   well-formed representation of the target type and `ParseRangeException`
//!   (§9.1; an `OverflowException`) when the value does not fit
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
