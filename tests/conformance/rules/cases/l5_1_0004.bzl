//! case: l5_1_0004
//! doc: stdlib
//! section: 5.1
//! title: Sum() accumulates an interface-typed receiver in the element's own type
//! rule: §5.1 declares `public extension IEnumerable<T> where T: INumber<T>` with
//!   `public method Sum(): owned T;` (stdlib §5.1, v0.340) — an accumulator over
//!   `T.Zero()` and `+`. `IList<T>` declares no instance member named `Sum`, so §26.5
//!   rule 1 does not commit instance resolution and rule 2 reaches the §5.1 extension
//!   block over `IEnumerable<T>`, whose block parameter is inferred as `int`. The sum of
//!   the elements 1, 2, 3 accumulates in `int` itself and is 6.
//! cites: 5.1, 26.5, 2.19
//! expect: run
//! stdout: 6
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var il: IList<int> := a;
    Console.WriteLine(il.Sum());
  end Main;
end Program;
