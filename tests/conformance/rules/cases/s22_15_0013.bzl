//! case: s22_15_0013
//! doc: spec
//! section: 22.15
//! title: pure is rejected on a static constructor
//! rule: `pure` is rejected on static constructors — no call site exists
//!   to consume the fact.
//! cites: 22.15, 22.13
//! expect: reject
//! diagnostic: static constructor
module Conf;

public class Config
  static pure constructor()
  end constructor;
end Config;
