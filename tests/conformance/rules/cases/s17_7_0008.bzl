//! case: s17_7_0008
//! doc: spec
//! section: 17.7
//! title: the new() shorthand is not admitted at an expression body
//! rule: "It is **not** permitted at an expression body (`=> new()`) or at a
//!   lambda's ascribed return."
//! cites: 17.7
//! expect: reject
//! diagnostic: shorthand
module Conf;

public class Config
  public constructor()
  end constructor;
end Config;

public class Factory
  public static method Make(): owned Config => new();
end Factory;
