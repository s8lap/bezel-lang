//! case: s28_5_0020
//! doc: spec
//! section: 28.5
//! title: owned on a field of a value-classified choice is rejected
//! rule: Only fields whose type is a **class type or an interface type** take
//!   the `owned` field modifier as a marking of their own. On a field of
//!   **owning-choice** type ... the modifier is **admitted-but-redundant**
//! cites: 28.5, 28.1, 35.4
//! expect: reject
//! diagnostic: owned
//! issue: 5898
module Conf;

public choice Code
  Num(n: int),
  Missing;
end Code;

public class Host
  private owned _code: Code := Code.Num(1);
end Host;
