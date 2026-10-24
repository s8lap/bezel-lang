//! case: s7_5_0018
//! doc: spec
//! section: 7.5
//! title: a null-conditional string member access short-circuits on a null receiver
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//!   `?.` short-circuits: the receiver is evaluated first; if it is null the
//!   access does not occur and nothing to its right in the chain is evaluated.
//!   `Substring` and `IndexOf` are declared members of `String` (stdlib §9.6),
//!   so `s?.Substring(0)` and `t?.IndexOf('c')` are member accesses the rule
//!   admits. The second control line's value is fixed by stdlib §9.6's own
//!   contract — `IndexOf` returns the code-point index of the first occurrence,
//!   or -1 — and not by the compiler: `Substring` has no result contract in
//!   §9.6, so a control line that printed a `Substring` result would pin
//!   compiler behaviour rather than spec behaviour.
//! cites: 7.5, 9.6
//! expect: run
//! output: null
//!   2
//! issue: 7130
module Conf;

public class Program
  public static method Main()
    var s: string? := null;
    Console.WriteLine($"{s?.Substring(0)}");
    var t: string? := "abcdef";
    Console.WriteLine($"{t?.IndexOf('c')}");
  end Main;
end Program;
