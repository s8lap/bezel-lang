//! case: s12_10_0001
//! doc: spec
//! section: 12.10
//! title: a class naming itself as its own base is a cycle
//! rule: The simplest case is a type naming itself (class C: C).
//! cites: 12.10
//! expect: reject
//! diagnostic: cycle
module Conf;

public class C: C
  public constructor()
  end constructor;
end C;
