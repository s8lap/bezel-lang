//! case: s7_7_0003
//! doc: spec
//! section: 7.7
//! title: mixing an owned arm and a borrowed arm of ?? is rejected
//! rule: When the operands are owned, `??` is a two-branch expression whose
//!   result ownership follows the meet rule of an if/else join: an owned arm
//!   mixed with a borrowed arm is rejected.
//! cites: 28.3
//! expect: reject
//! diagnostic: borrowed arm
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Choose(fallback: Widget?)
    using var w := fallback ?? new Widget();
  end Choose;
end Program;
