//! case: s8_1_0002
//! doc: spec
//! section: 8.1
//! title: a class named MyObject is an ordinary declaration
//! rule: `Object` is a compiler intrinsic; user code cannot redeclare or
//!   replace `Object`. A type with any other name is unaffected.
//! cites: 8.1
//! expect: accept
module Conf;

public class MyObject
  public constructor()
  end constructor;
end MyObject;
