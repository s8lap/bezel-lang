//! case: s16_6_0006
//! doc: spec
//! section: 16.6
//! title: a closed ulong range pattern compares unsigned across 2^63 and includes its upper
//!   bound
//! rule: A range pattern `lo..hi` / `lo..<hi` "matches `v` iff `lo <= v <= hi` (resp. `< hi`)
//!   read directionally as written" and is admitted over "the integer types and `char`"
//!   (§16.6, v0.340). For a `ulong` scrutinee the comparison is unsigned, so
//!   `9223372036854775808..18446744073709551615` holds both its endpoints and nothing below
//!   2^63.
//! cites: 19.3, 5.1
//! expect: run
//! output: low
//!   low
//!   high
//!   high
module Conf;

public class Program
  public static method Classify(u: ulong): string
    return (case u of
      0..9223372036854775807UL => "low",
      9223372036854775808UL..18446744073709551615UL => "high",
      _ => "none");
  end Classify;

  public static method Main()
    Console.WriteLine(Classify(0UL));
    Console.WriteLine(Classify(9223372036854775807UL));
    Console.WriteLine(Classify(9223372036854775808UL));
    Console.WriteLine(Classify(18446744073709551615UL));
  end Main;
end Program;
