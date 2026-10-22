//! case: s14_5_0020
//! doc: spec
//! section: 14.5
//! title: under an explicit byte list a non-constant int argument does not narrow even when its value fits
//! rule: With an explicit list — Pick<byte>(b, 250) — the parameter type is byte
//!   and the literal constant-narrows at the argument conversion site as usual.
//!   Between signed and unsigned types of the same or narrower width (e.g. int
//!   → uint, uint → int, short → ushort). ... This bullet governs non-constant
//!   expressions; an integer compile-time constant (§17.14) converts by value
//!   under constant narrowing (below), which may cross signedness
//! cites: 14.5, 5.1
//! expect: reject
//! diagnostic: same or narrower width
module Conf;

public class Program
  public static method Main()
    var i: int := 200;
    Console.WriteLine(Math.Max<byte>(i, 3));
  end Main;
end Program;
