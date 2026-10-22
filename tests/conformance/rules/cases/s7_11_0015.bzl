//! case: s7_11_0015
//! doc: spec
//! section: 7.11
//! title: a user-defined binary operator rejects a nullable record right operand that is not narrowed
//! rule: A user-declared `operator` on a record or class type (§25) is likewise not
//!   defined on a nullable operand: such an operand at a user-defined-operator
//!   position is the same compile error, naming the same remedies.
//! cites: 7.11, 25.2
//! expect: reject
//! diagnostic: nullable
module Conf;

public record Pt(X: int, Y: int)
  public static pure method operator +(a: Pt, b: Pt): Pt => new Pt(a.X + b.X, a.Y + b.Y);
end Pt;

public static class Util
  public static method Add(a: Pt, p: Pt?): int
    var s := a + p;
    return s.X;
  end Add;
end Util;
