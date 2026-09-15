//! case: s22_6_0011
//! doc: spec
//! section: 22.6
//! title: a non-generic candidate is preferred over a generic one on a tie
//! rule: "When a non-generic candidate and a generic candidate are both
//!   applicable, and neither the ranking ... nor the defaulted-arity
//!   tie-break separates them, the non-generic candidate is selected ...
//!   `Show(x: int)` and `Show<T>(x: T)` on `Show(5)` resolve to the
//!   non-generic."
//! cites: 22.6
//! expect: run
//! stdout: int
module Conf;

public class Sample
  public constructor()
  end constructor;

  public method Show(x: int): string => "int";
  public method Show<T>(x: T): string => "generic";
end Sample;

public class Program
  public static method Main()
    var s := new Sample();
    Console.WriteLine(s.Show(5));
  end Main;
end Program;
