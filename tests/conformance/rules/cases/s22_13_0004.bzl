//! case: s22_13_0004
//! doc: spec
//! section: 22.13
//! title: a static constructor body may freely reference fields in any order
//! rule: A static constructor body may freely reference any of the
//!   type's static fields in any order; the constructor body runs after
//!   all field initializers have completed, so no forward-reference
//!   question arises there.
//! cites: 22.13
//! expect: accept
module Conf;

public class Config
  private static readonly _a: int := 1;
  private static readonly _b: int := 2;
  private static _sum: int := 0;

  static constructor()
    _sum := _b + _a;
  end constructor;
end Config;
