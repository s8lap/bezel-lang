//! case: s20_9703
//! doc: spec
//! section: 20
//! title: a lambda whose expression body is a conditional of two method groups returns the selected method
//! rule: the common type of branch types A and B is whichever of the two the other implicitly converts
//!   into — identical types trivially
//! cites: 20, 27.3, 27.6, 27.7
//! expect: run
//! output: 11
//! notes: Pins #9215 (verified already fixed by #9155 d3f56e723; the leak halves are #8892 / #8175 and are
//!   not asserted): the two arms are method references whose formed types (27.6) are identical, so
//!   the conditional has that common type and the value is a function value. With c = true the then-
//!   arm P is selected: P(1) = 1 + 10 = 11 (the else-arm Q would give 3). The lambda's expression
//!   body is the conditional (the issue's verifyModule failure): the lambda returns the common type
//!   `(int) -> int`, and mk(true) yields P.
module Conf;

public class Program
  static method P(i: int): int => i + 10;
  static method Q(i: int): int => i * 3;
  public static method Main(): int
    var mk := lambda(c: bool) => (if c then P else Q);
    var f := mk(true);
    var n := f(1);
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
