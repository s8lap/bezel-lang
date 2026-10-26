//! case: s22_6_0111
//! doc: spec
//! section: 22.6
//! title: a constant the narrowing reaches at two candidates is ambiguous
//! rule: spec §22.6: "§5.1's constant narrowing enters resolution as an
//!     *applicability* rule only: a candidate the narrowing reaches *converts*
//!     at that position, never exact, and ranks co-equal with every other
//!     conversion. So ... `M(3)` against `M(byte)` beside `M(short)` is
//!     ambiguous — both candidates convert — remedied as every ambiguity is, by
//!     a cast".
//! cites: 22.6, 5.1
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Program
  public static method M(x: byte): string
    return "byte";
  end M;

  public static method M(x: short): string
    return "short";
  end M;

  public static method Main()
    Console.WriteLine(M(3));
  end Main;
end Program;
