//! case: s29_3_0008
//! doc: spec
//! section: 29.3
//! title: named record destructure bindings of a case arm are read after yields in an iterator
//!   method
//! rule: "`yield return expression;` produces the next value and suspends" (§29.3, v0.340); the
//!   property bindings `x` and `y` of the destructure arm (§19.3) are read after earlier
//!   `yield return` statements of the same arm, so the iterator yields `x`, `y`, then `x *
//!   y`.
//! cites: 19.3, 9.1
//! expect: run
//! output: 3
//!   4
//!   12
//!   -1
module Conf;

public record Pt(X: int, Y: int);

public class Program
  public static method G(p: Pt): IEnumerable<int>
    case p of
      Pt(X = var x, Y = var y) when x > 0 => do yield return x; yield return y; yield return x * y; end,
      _ => yield return -1;
    end;
  end G;

  public static method Main()
    for var v in G(new Pt(3, 4)) do
      Console.WriteLine(v);
    end;
    for var v in G(new Pt(0, 4)) do
      Console.WriteLine(v);
    end;
  end Main;
end Program;
