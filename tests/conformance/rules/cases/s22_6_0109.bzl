//! case: s22_6_0109
//! doc: spec
//! section: 22.6
//! title: an exact Math.Abs overload is chosen and a float argument has a single applicable
//!   candidate
//! rule: "the one that is exact is better" (§22.6, v0.340): `Math.Abs` on an `int`, `long`,
//!   `double` or `decimal` argument selects the identity overload. A `float` converts only
//!   to `double` among the four (§5.1), so that is the sole applicable candidate and no
//!   ambiguity arises.
//! cites: 5.1
//! expect: run
//! output: 5
//!   6
//!   2.5
//!   3.5
//!   1.5
module Conf;

public class Program
  public static method Main()
    var i: int := -5;
    var l: long := -6;
    var d: double := -2.5;
    var m: decimal := -3.5m;
    var f: float := -1.5f;
    Console.WriteLine(Math.Abs(i));
    Console.WriteLine(Math.Abs(l));
    Console.WriteLine(Math.Abs(d));
    Console.WriteLine(Math.Abs(m));
    Console.WriteLine(Math.Abs(f));
  end Main;
end Program;
