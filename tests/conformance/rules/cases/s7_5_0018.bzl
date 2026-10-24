//! case: s7_5_0018
//! doc: spec
//! section: 7.5
//! title: a null-conditional string method call short-circuits on a null receiver
//! rule: `?.` short-circuits. The receiver is evaluated first; if it is null
//!   the access does not occur and nothing to its right in the chain is
//!   evaluated. `Substring` is a declared member of `String` (stdlib §9.6), so
//!   `s?.Substring(0)` is a member access the rule admits.
//! cites: 7.5, 9.6
//! expect: run
//! output: null
//!   ab
//! issue: 7130
module Conf;

public class Program
  public static method Main()
    var s: string? := null;
    Console.WriteLine($"{s?.Substring(0)}");
    var t: string? := "abcdef";
    Console.WriteLine($"{t?.Substring(0, 2)}");
  end Main;
end Program;
