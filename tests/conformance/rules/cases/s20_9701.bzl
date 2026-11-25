//! case: s20_9701
//! doc: spec
//! section: 20
//! title: an immediately invoked conditional of two method groups selects by the condition
//! rule: the common type of branch types A and B is whichever of the two the other implicitly converts
//!   into — identical types trivially
//! cites: 20, 27.6, 27.7, 28.9
//! expect: run
//! output: 11
//! notes: Pins #9215 (verified already fixed by #9155 d3f56e723; the leak halves are #8892 / #8175 and are
//!   not asserted): the two arms are method references whose formed types (27.6) are identical, so
//!   the conditional has that common type and the value is a function value. With c = true the then-
//!   arm P is selected: P(1) = 1 + 10 = 11 (the else-arm Q would give 3). The conditional is called
//!   at once (the issue's second spelling); the result is bound before it is printed.
module Conf;

public class Program
  static method P(i: int): int => i + 10;
  static method Q(i: int): int => i * 3;
  public static method Main(): int
    var c := true;
    var n := (if c then P else Q)(1);
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
