//! case: s28_8_1_0024
//! doc: spec
//! section: 28.8.1
//! title: with followed by a parenthesized assignment list after a move operand is the record update
//! rule: A `with` followed by `(` and `Identifier :=` is the record-update
//!   suffix and binds to the operand: `move r with(X := 1)` is
//!   `move (r with(X := 1))`, a `move` of a record value, rejected by this
//!   section's operand rule, the diagnostic naming the replacing form with
//!   its replacement parenthesized, `move loc with (e)`.
//! cites: 28.8.1, 9.5
//! expect: reject
//! diagnostic: move loc with
//! issue: 5923
module Conf;

public record Pt(X: int, Y: int);

public class Program
  public method Demo(): int
    var r := new Pt(0, 2);
    var t := move r with(X := 1);
    return t.X;
  end Demo;
end Program;
