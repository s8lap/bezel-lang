//! case: l9_1_4805
//! doc: stdlib
//! section: 9.1
//! title: long.TryParse returns false with out = 0 for an out-of-range radix-10 input and true at the maximum
//! rule: // shape (c): false (out = 0) // exactly where Parse(s, radix) raises // FormatException/OverflowException;
//! cites: 1, 2.3
//! expect: run
//! output: True:9223372036854775807
//!   False:0
module Conf;

public class Program
  public static method T(s: string, r: int): string
    var v: long;
    var ok := long.TryParse(s, r, out v);
    return ok.ToString() + ":" + v.ToString();
  end T;

  public static method Main()
    Console.WriteLine(T("9223372036854775807", 10));
    Console.WriteLine(T("9223372036854775808", 10));
  end Main;
end Program;
