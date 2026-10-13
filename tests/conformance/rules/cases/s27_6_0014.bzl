//! case: s27_6_0014
//! doc: spec
//! section: 27.6
//! title: a reference to Math.Max with its list, and to the non-generic Math.Abs, are formed
//! rule: Overload selection follows §14.5's list rule: with a list, only
//!   candidates declaring that many type parameters of their own take part;
//!   without one, only candidates declaring none take part, and the error
//!   above is reported where every candidate is generic (`var f: (int) -> int
//!   := Math.Abs;` selects among `Abs`'s non-generic overloads; `Math.Max`
//!   must be written `Math.Max<int>`).
//! cites: 27.6, 14.5
//! expect: run
//! output: 5
//!   4
//! issue: 5814
module Conf;

public class Program
  public static method Main()
    var m: (int, int) -> int := Math.Max<int>;
    Console.WriteLine(m(3, 5));
    var a: (int) -> int := Math.Abs;
    Console.WriteLine(a(-4));
  end Main;
end Program;
