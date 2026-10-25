//! case: l2_2_0016
//! doc: stdlib
//! section: 2.2
//! title: a Math member's supplied arguments run once each, in call-site textual order
//! rule: stdlib §2.2 declares `Pow(x: double, y: double): double`. §22.5, Evaluation
//!   order: a call's supplied argument expressions are evaluated in call-site
//!   textual order - left to right as written, for positional and named arguments
//!   alike; naming an argument changes which parameter it binds to, never when it
//!   runs. In `Pow(y := Next("y", 1), x := Next("x", 2))` the `y` expression runs
//!   first, the `x` expression second, each once - and the call is `Pow(x: 2, y:
//!   1)`, which is 2.
//! cites: 2.2, 22.5
//! expect: run
//! output: y
//!   x
//!   2
//! issue: 7219
module Conf;

public class Marker
  public method Next(tag: string, value: int): int
    Console.WriteLine(tag);
    return value;
  end Next;
end Marker;

public class Program
  public static method Main()
    var m := new Marker();
    Console.WriteLine(Math.Pow(y := m.Next("y", 1), x := m.Next("x", 2)).ToString());
  end Main;
end Program;
