//! case: s22_6_0012
//! doc: spec
//! section: 22.6
//! title: the defaulted-arity tie-break applies before the non-generic preference
//! rule: "the defaulted-arity tie-break applies first: `M(n: int, m: int :=
//!   0)` and `M<T>(x: T)` on the call `M(5)` resolve to the *generic*
//!   candidate — it fills no default."
//! cites: 22.6
//! expect: run
//! stdout: generic
module Conf;

public class Sample
  public constructor()
  end constructor;

  public method M(n: int, m: int := 0): string => "defaulted";
  public method M<T>(x: T): string => "generic";
end Sample;

public class Program
  public static method Main()
    var s := new Sample();
    Console.WriteLine(s.M(5));
  end Main;
end Program;
