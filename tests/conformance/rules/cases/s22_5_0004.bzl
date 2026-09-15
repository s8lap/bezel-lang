//! case: s22_5_0004
//! doc: spec
//! section: 22.5
//! title: a ref or out argument has no named spelling
//! rule: "A `ref`/`out`/`scoped` argument is positional-only: those
//!   modifiers inhabit the `ArgModifiers` alternative of `Argument`, which
//!   has no named form — `M(out var result := n)` has no reading and does
//!   not parse, the `:=` of the binding form and the `:=` of the named form
//!   being the same token."
//! cites: 22.5
//! expect: reject
//! diagnostic: does not parse
module Conf;

public class Parser
  public static method TryParse(s: string, out result: int): bool
    result := 0;
    return false;
  end TryParse;
end Parser;

public class Program
  public static method Main()
    var n := 5;
    var ok := Parser.TryParse("x", out var result := n);
  end Main;
end Program;
