//! case: s22_6_0010
//! doc: spec
//! section: 22.6
//! title: the defaulted-arity tie-break prefers the candidate that fills no default
//! rule: "A candidate applicable **without filling any default** is
//!   preferred over a candidate that fills one or more ... `M(5)` against
//!   `M(n: int)` and `M(n: int, m: int := 0)` resolves to `M(int)`."
//! cites: 22.6, 22.5
//! expect: run
//! stdout: one
module Conf;

public class Sample
  public constructor()
  end constructor;

  public method M(n: int): string => "one";
  public method M(n: int, m: int := 0): string => "two";
end Sample;

public class Program
  public static method Main()
    var s := new Sample();
    Console.WriteLine(s.M(5));
  end Main;
end Program;
