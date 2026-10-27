//! case: l2_18_0020
//! doc: stdlib
//! section: 2.18
//! title: a run-time format string renders as the literal spelling of the same format
//! rule: stdlib §9.1 declares `public pure method ToString(format: string):
//!     string` — one member, taking the format as a value — and §2.18's integer
//!     row gives the rendering of `D`/`Dn`. The format is the same string
//!     whichever spelling supplies it, so the two renderings agree.
//! cites: 2.18, 9.1
//! expect: run
//! output: 0255
//!   0255
//! issue: 7286
module Conf;

public class Program
  public static method Main()
    var f: string := "D4";
    Console.WriteLine((255).ToString("D4"));
    Console.WriteLine((255).ToString(f));
  end Main;
end Program;
