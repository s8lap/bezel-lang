//! case: s17_3_0001
//! doc: spec
//! section: 17.3
//! title: a class cannot overload the = operator
//! rule: "`=` and `<>` invoke `Equals` with a null-safe wrapper. For classes,
//!   `=` cannot be overloaded; for records and enums, it can." §25.2: "`=` and
//!   `<>` may **not** be overloaded on classes ... For classes, `=` always
//!   invokes `Equals`."
//! cites: 17.3, 25.1, 25.2
//! expect: reject
//! diagnostic: overloaded on classes
module Conf;

public class Box
  private readonly _v: int;

  public constructor(v: int)
    _v := v;
  end constructor;

  public static method operator =(a: Box, b: Box): bool => a._v = b._v;
  public static method operator <>(a: Box, b: Box): bool => a._v <> b._v;
end Box;
