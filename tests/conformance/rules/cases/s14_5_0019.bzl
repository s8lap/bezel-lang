//! case: s14_5_0019
//! doc: spec
//! section: 14.5
//! title: under an explicit byte list an int literal beside a byte local constant-narrows at the argument site
//! rule: With an explicit list — Pick<byte>(b, 250) — the parameter type is byte
//!   and the literal constant-narrows at the argument conversion site as usual.
//! cites: 14.5, 5.1
//! expect: run
//! output: 250
//!   100
module Conf;

public class Program
  public static method Main()
    var b: byte := 100;
    Console.WriteLine(Math.Max<byte>(b, 250));
    Console.WriteLine(Math.Min<byte>(b, 250));
  end Main;
end Program;
