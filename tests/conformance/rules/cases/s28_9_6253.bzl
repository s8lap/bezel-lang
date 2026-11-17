//! case: s28_9_6253
//! doc: spec
//! section: 28.9
//! title: a trivial-dispose declared type over a destructor-bearing coalesce
//!   production is not exempt
//! rule: a trivial-dispose *declared* type standing over a
//!   destructor-bearing production
//! cites: 28.9, 28.3
//! expect: warn
//! diagnostic: must-use
module Conf;

class Base
  public constructor() end constructor;
  public method X(): int => 1;
end Base;

class Derived: Base
  public constructor() base(); end constructor;
  destructor end destructor;
end Derived;

class C
  private static method MkDN(): owned Derived? => new Derived();

  public static method Run(): int
    var b: Base := MkDN() ?? new Derived();
    return b.X();
  end Run;
end C;
