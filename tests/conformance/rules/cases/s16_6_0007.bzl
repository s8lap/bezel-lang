//! case: s16_6_0007
//! doc: spec
//! section: 16.6
//! title: a half-open range and a relational pattern over ulong read the unsigned order above
//!   2^63
//! rule: A range pattern `lo..hi` / `lo..<hi` "matches `v` iff `lo <= v <= hi` (resp. `< hi`)
//!   read directionally as written" and is admitted over "the integer types and `char`"
//!   (§16.6, v0.340). The half-open form excludes only its upper bound, and a relational
//!   pattern compares "under the intrinsic ordering of the scrutinee's type" (§19.3), which
//!   for `ulong` is unsigned.
//! cites: 19.3
//! expect: run
//! output: in
//!   out
//!   in
//!   out
module Conf;

public class Program
  public static method Half(u: ulong): string
    return (case u of
      0..<9223372036854775809UL => "in",
      _ => "out");
  end Half;

  public static method Rel(u: ulong): string
    return (case u of
      >= 9223372036854775808UL => "in",
      _ => "out");
  end Rel;

  public static method Main()
    Console.WriteLine(Half(9223372036854775808UL));
    Console.WriteLine(Half(9223372036854775809UL));
    Console.WriteLine(Rel(18446744073709551615UL));
    Console.WriteLine(Rel(9223372036854775807UL));
  end Main;
end Program;
