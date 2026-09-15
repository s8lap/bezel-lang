//! case: s8_1_0001
//! doc: spec
//! section: 8.1
//! title: user code cannot redeclare Object
//! rule: `Object` is a compiler intrinsic; method bodies are provided by the
//!   runtime. User code cannot redeclare or replace `Object`.
//! cites: 8.1
//! expect: reject
//! diagnostic: redeclare
module Conf;

public class Object
  public constructor()
  end constructor;
end Object;
