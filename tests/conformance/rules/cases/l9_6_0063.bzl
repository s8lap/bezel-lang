//! case: l9_6_0063
//! doc: stdlib
//! section: 9.6
//! title: ToUpper maps a non-ASCII code point by the simple case mapping
//! rule: Each maps every code point of the receiver by `Char.ToUpper` or
//!   `Char.ToLower` (§9.5): the simple Unicode case mapping, one code point to
//!   one, with no locale tailoring. So `"éa".ToUpper()` is `"ÉA"`, a code
//!   point with no mapping (`日`) is kept, and the result's `Length` is the
//!   receiver's.
//! cites: 9.6, 9.5
//! expect: run
//! stdout: ÉA 2
//! issue: 7473
module Conf;

public class Program
  public static method Main()
    var u := "\u00E9a".ToUpper();
    Console.WriteLine($"{u} {u.Length}");
  end Main;
end Program;
