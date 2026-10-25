//! case: l2_2_0015
//! doc: stdlib
//! section: 2.2
//! title: a reordered named argument on a Math member binds to the parameter it names
//! rule: stdlib §2.2 declares `Round(x: double, digits: int := 0): double` and
//!   `IEEERemainder(x: double, y: double): double`. §22.5: naming an argument
//!   changes which parameter it binds to, never when it runs - so
//!   `Round(digits := 3, x := 2.5)` is `Round(x: 2.5, digits: 3)` and renders
//!   2.5, and `IEEERemainder(y := 4, x := 10)` is `IEEERemainder(x: 10, y: 4)`,
//!   which is 10 - 2*4 = 2 (the integer n nearest 10/4 is 2).
//! cites: 2.2, 22.5
//! expect: run
//! output: 2.5
//!   2
//! issue: 7199
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Round(digits := 3, x := 2.5).ToString());
    Console.WriteLine(Math.IEEERemainder(y := 4, x := 10).ToString());
  end Main;
end Program;
