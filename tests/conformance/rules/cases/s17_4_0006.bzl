//! case: s17_4_0006
//! doc: spec
//! section: 17.4
//! title: the unit type as target is rejected
//! rule: "The **unit type** is likewise a compile error as an `as` target:
//!   `()` has exactly one value, so the operation could never carry more
//!   information than `x is ()`."
//! cites: 17.4
//! expect: reject
//! diagnostic: unit
module Conf;

public class Program
  public static method Check(o: Object): bool
    var r := o as ();
    return r <> null;
  end Check;
end Program;
