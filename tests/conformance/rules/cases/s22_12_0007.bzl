//! case: s22_12_0007
//! doc: spec
//! section: 22.12
//! title: a mutable static field of an ordinary class type is rejected
//! rule: any other class type — `owned` or not — takes: mutable: error †
//!   — a mutable static admitted by none of (i)-(iv) is a compile error
//!   naming this rule.
//! cites: 22.12
//! expect: reject
//! diagnostic: cross-domain
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Registry
  private static _current: Widget := new Widget();
end Registry;
