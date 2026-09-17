//! case: s17_2_0012
//! doc: spec
//! section: 17.2
//! title: range mixed with bitwise AND, parenthesized on the bitwise operand, is admitted
//! rule: "write `(mask & 0)..n` to disambiguate" — a parenthesization restores
//!   a parse, and the result is then subject to the ordinary operand rules;
//!   `(mask & 0)..n` is the one shown grouping, `&` being defined on the
//!   integer types and `Range<T>` declaring no operators.
//! cites: 17.2
//! expect: accept
module Conf;

public class Ops
  public constructor()
  end constructor;

  public method Make(mask: int, n: int)
    var r := (mask & 0)..n;
  end Make;
end Ops;
