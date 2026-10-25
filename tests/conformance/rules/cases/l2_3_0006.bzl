//! case: l2_3_0006
//! doc: stdlib
//! section: 2.3
//! title: a Convert member's supplied arguments run once each, in call-site textual order
//! rule: stdlib §2.3 declares `ToString(value: int, toBase: int): string` and states
//!   that the result is the text `value.ToString(toBase)` returns (§9.1), whose
//!   radix digits are lower-case. §22.5, Evaluation order: a call's supplied
//!   argument expressions are evaluated in call-site textual order - left to
//!   right as written, for positional and named arguments alike; naming an
//!   argument changes which parameter it binds to, never when it runs. In
//!   `ToString(toBase := Next("tb", 16), value := Next("v", 255))` the `toBase`
//!   expression runs first, the `value` expression second, each once, and the
//!   call renders "ff".
//! cites: 2.3, 22.5, 9.1
//! expect: run
//! output: tb
//!   v
//!   ff
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
    Console.WriteLine(Convert.ToString(toBase := m.Next("tb", 16), value := m.Next("v", 255)));
  end Main;
end Program;
