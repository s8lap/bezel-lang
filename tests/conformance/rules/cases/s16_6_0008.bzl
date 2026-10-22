//! case: s16_6_0008
//! doc: spec
//! section: 16.6
//! title: a named ulong constant at the upper bound of a range pattern draws no diagnostic
//! rule: A range pattern `lo..hi` / `lo..<hi` "matches `v` iff `lo <= v <= hi` (resp. `< hi`)
//!   read directionally as written" and is admitted over "the integer types and `char`"
//!   (§16.6, v0.340). The bounds `0` and `Big` (the `ulong` maximum) are in order, so the
//!   pattern is not reversed and the compiler has nothing to say.
//! cites: 19.3
//! expect: quiet
module Conf;

public class Program
  public const Big: ulong := 18446744073709551615UL;

  public static method InRange(u: ulong): bool
    return (case u of
      0..Big => true,
      _ => false);
  end InRange;
end Program;
