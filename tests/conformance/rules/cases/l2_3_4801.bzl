//! case: l2_3_4801
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToInt64 accepts the maximum and raises the range failure for a larger value or a smaller one
//! rule: The `To*` methods raise `FormatException` when the string is not a well-formed representation of the target type and `ParseRangeException` ([§9.1](#sec-integer-types); an `OverflowException`) when the value does not fit
//! cites: 9.1
//! expect: run
//! output: ok:9223372036854775807
//!   OVF
//!   OVF
module Conf;

public class Program
  public static method P(s: string): string
    try
      var v := Convert.ToInt64(s);
      return "ok:" + v.ToString();
    catch
      FormatException => do return "FMT"; end,
      OverflowException => do return "OVF"; end
    end try;
  end P;

  public static method Main()
    Console.WriteLine(P("9223372036854775807"));
    Console.WriteLine(P("9223372036854775808"));
    Console.WriteLine(P("-9223372036854775809"));
  end Main;
end Program;
