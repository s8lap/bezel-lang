//! case: s22_16_0019
//! doc: spec
//! section: 22.16
//! title: a static requires clause may mention only state its call sites can see
//! rule: A `static requires` clause may mention only state its call
//!   sites can see (the member's parameters, and members visible to
//!   every caller); a `static requires` over non-public state is a
//!   compile error at the declaration.
//! cites: 22.16
//! expect: reject
//! diagnostic: non-public
module Conf;

public class Widget
  private static _limit: int := 10;

  public static method Check(x: int): bool
    static requires(x < _limit)

    return true;
  end Check;
end Widget;
