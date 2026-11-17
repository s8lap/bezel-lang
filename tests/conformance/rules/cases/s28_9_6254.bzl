//! case: s28_9_6254
//! doc: spec
//! section: 28.9
//! title: a plain var bound by move from a trivial-dispose owned local draws
//!   no must-use warning
//! rule: No **must-use / auto-drop diagnostic** attaches to a
//!   trivial-dispose value.
//! cites: 28.9, 28.3
//! expect: quiet
module Conf;

class Base
  public constructor() end constructor;
  public method X(): int => 1;
end Base;

class C
  public static method Run(): int
    var d := new Base();
    var b: Base := move d;
    return b.X();
  end Run;
end C;
