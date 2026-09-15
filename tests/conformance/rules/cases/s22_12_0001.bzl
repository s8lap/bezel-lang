//! case: s22_12_0001
//! doc: spec
//! section: 22.12
//! title: static methods, properties, fields, and constructors are declared with static
//! rule: Static methods, properties, fields, and constructors are
//!   declared with `static`.
//! cites: 22.12
//! expect: accept
module Conf;

public class Registry
  private static _count: int := 0;

  static constructor()
    _count := 0;
  end constructor;

  public static property Count: int => _count;

  public static method Bump()
    _count := _count + 1;
  end Bump;
end Registry;
