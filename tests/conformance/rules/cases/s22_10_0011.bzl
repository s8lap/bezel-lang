//! case: s22_10_0011
//! doc: spec
//! section: 22.10
//! title: a scoped primary-constructor parameter is rejected
//! rule: A `scoped` primary-constructor parameter is rejected in this
//!   revision: the header admits plain and `owned` parameters only.
//! cites: 22.10
//! expect: reject
//! diagnostic: scoped
module Conf;

public class Owner
  public constructor()
  end constructor;
end Owner;

public class Box(scoped owner: Owner)
  public method Describe(): string => "box";
end Box;
