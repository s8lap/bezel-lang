//! case: s17_2_0001
//! doc: spec
//! section: 17.2
//! title: bitwise AND mixed with equality without parentheses is rejected
//! rule: Bitwise (`&`, `|`, `^`) with equality (`=`, `<>`) or relational
//!   (`<`, `<=`, `>`, `>=`) is non-associative across categories and requires
//!   explicit parenthesization when mixed; `a & 1 = 0` is a compile error.
//! cites: 17.2
//! expect: reject
//! diagnostic: parenthes
module Conf;

public class Ops
  public constructor()
  end constructor;

  public method IsEven(a: int): bool
    var r := a & 1 = 0;
    return r;
  end IsEven;
end Ops;
