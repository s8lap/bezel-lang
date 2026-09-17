//! case: s6_6_0004
//! doc: spec
//! section: 6.6
//! title: ++ is not a user-overloadable operator token
//! rule: Unary operators have no compound forms; ++/-- are not overloadable
//!   (v0.289) — declare operator +, write c := c + 1;.
//! cites: 6.6, 17.6, 25.1
//! expect: reject
//! diagnostic: not overloadable
module Conf;

public class Counter
  public constructor()
  end constructor;

  public static pure method operator ++(c: Counter): Counter => c;
end Counter;
