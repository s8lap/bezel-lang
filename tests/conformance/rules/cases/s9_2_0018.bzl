//! case: s9_2_0018
//! doc: spec
//! section: 9.2
//! title: override is admitted on ToString and replaces the compiler-generated rendering
//! rule: override is admitted only on Equals, GetHashCode, and ToString; an
//!   override here names the Object slot the member fills and implies no chain.
//! expect: run
//! stdout: Sample<7>
module Conf;

public record Sample
  public readonly property N: int;

  public constructor(n: int)
    N := n;
  end constructor;

  public override method ToString(): string => $"Sample<{N}>";
end Sample;

public class Program
  public static method Main()
    var s := new Sample(7);
    Console.WriteLine(s.ToString());
  end Main;
end Program;
