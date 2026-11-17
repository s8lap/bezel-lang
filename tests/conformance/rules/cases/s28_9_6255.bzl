//! case: s28_9_6255
//! doc: spec
//! section: 28.9
//! title: a plain var bound by move from an owned parameter of
//!   trivial-dispose type draws no must-use warning
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
  public static method Take(owned p: Base): int
    var b := move p;
    return b.X();
  end Take;
end C;
