//! case: s28_9_6251
//! doc: spec
//! section: 28.9
//! title: a plain var from the null-forgiving read of a trivial-dispose
//!   owned call draws no must-use warning
//! rule: No **must-use / auto-drop diagnostic** attaches to a
//!   trivial-dispose value.
//! cites: 28.9, 28.3
//! expect: quiet
module Conf;

class Base
  public constructor() end constructor;
  public method X(): int => 1;
end Base;

class Derived: Base
  public constructor() base(); end constructor;
end Derived;

class C
  private static method MkDN(): owned Derived? => new Derived();

  public static method Run(): int
    var b: Base := MkDN()!;
    return b.X();
  end Run;
end C;
