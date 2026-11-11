//! case: l9_1_4804
//! doc: stdlib
//! section: 9.1
//! title: a malformed input with an out-of-range digit prefix raises FormatException, not the range failure
//! rule: any other input raises `FormatException`
//! cites: 2.3
//! expect: run
//! output: FMT
//!   FMT
module Conf;

public class Program
  public static method P(s: string, r: int): string
    try
      var v := long.Parse(s, r);
      return "ok:" + v.ToString();
    catch
      FormatException => do return "FMT"; end,
      OverflowException => do return "OVF"; end
    end try;
  end P;

  public static method Main()
    Console.WriteLine(P("9223372036854775808x", 10));
    Console.WriteLine(P("", 10));
  end Main;
end Program;
