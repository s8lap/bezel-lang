//! case: s17_2_0003
//! doc: spec
//! section: 17.2
//! title: bitwise AND mixed with a relational operator without parentheses is rejected
//! rule: Bitwise (`&`, `|`, `^`) with relational (`<`, `<=`, `>`, `>=`) is
//!   non-associative across categories and requires explicit
//!   parenthesization when mixed.
//! cites: 17.2
//! expect: reject
//! diagnostic: parenthes
module Conf;

public class Ops
  public constructor()
  end constructor;

  public method Check(a: int): bool
    var r := a & 1 < 2;
    return r;
  end Check;
end Ops;
