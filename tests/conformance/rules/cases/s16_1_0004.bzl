//! case: s16_1_0004
//! doc: spec
//! section: 16.1
//! title: parenthesizing the bitwise operand admits the bitwise-then-range mix
//! rule: The smallest legal edit of s16_1_0003 — grouping the bitwise operation
//!   inside its own parentheses ("write `(mask & 0)..n`") resolves the mandatory-
//!   parenthesization category mix and the expression type-checks as `int..int`.
//! cites: 16.1, 17.2
//! expect: accept
module Conf;

public class Program
  private static method Compute()
    var mask: int := 12;
    var n: int := 5;
    var r := (mask & 0)..n;
  end Compute;
end Program;
