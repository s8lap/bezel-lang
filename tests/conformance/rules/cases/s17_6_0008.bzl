//! case: s17_6_0008
//! doc: spec
//! section: 17.6
//! title: operator ++ is not user-overloadable
//! rule: "`++`/`--` are not user-overloadable (§25.1), so a type spells
//!   advancement as `x := x + 1` under its own `operator +`, or as a named
//!   method."
//! cites: 17.6, 25.1
//! expect: reject
//! diagnostic: overload
module Conf;

public record Counter(N: int)
  public static method operator ++(c: Counter): Counter => new Counter(c.N + 1);
end Counter;
