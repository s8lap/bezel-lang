//! case: s28_3_8_8603
//! doc: spec
//! section: 28.3.8
//! title: an int ref beside a ref holding a call-bounded function value is admitted at an Object parameter
//! rule: A `ref` or plain `out` argument location ([§22.3](#sec-ref-and-out-argument-locations)) whose type, after substitution, can carry a borrow — a class or interface type, a function type, or a borrow-bearing record, choice or inline array
//! cites: 28.3.8, 22.3
//! expect: run
//! stdout: 2
//! notes: Harvested from the #9075 review (probe a02, the over-rejection the fix-forward removed).
//!   g holds the plain function-typed parameter q (a non-empty set) and is passed `ref` beside
//!   the int local n. An int location cannot carry a borrow, so the call's sources do not reach
//!   it, and Show(n) passes a boxed int, not a function value, at the Object parameter. Mix
//!   increments n once, so the output is 2.
module Conf;
import System;
public class Program
  public static method Mix(ref n: int, ref a: () -> int)
    n := n + 1;
  end Mix;
  public static method Show(o: Object): string
    return o.ToString();
  end Show;
  public static method Via(q: () -> int): string
    var n: int := 1;
    var g: () -> int := q;
    Program.Mix(ref n, ref g);
    return Program.Show(n);
  end Via;
  public static method Main(): int
    var c := 40;
    Console.WriteLine(Program.Via(lambda(): int => c));
    return 0;
  end Main;
end Program;
