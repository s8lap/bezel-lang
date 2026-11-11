//! case: l2_3_4802
//! doc: stdlib
//! section: 2.3
//! title: Convert.TryToInt64 is false with out = 0 exactly where ToInt64 raises, and true at the minimum
//! rule: `Try` forms return `false` in exactly the cases the throwing forms would raise; the `out` parameter receives the default value.
//! cites: 9.1
//! expect: run
//! output: True:-9223372036854775808
//!   False:0
//!   False:0
module Conf;

public class Program
  public static method T(s: string): string
    var v: long;
    var ok := Convert.TryToInt64(s, out v);
    return ok.ToString() + ":" + v.ToString();
  end T;

  public static method Main()
    Console.WriteLine(T("-9223372036854775808"));
    Console.WriteLine(T("-9223372036854775809"));
    Console.WriteLine(T("9223372036854775808x"));
  end Main;
end Program;
