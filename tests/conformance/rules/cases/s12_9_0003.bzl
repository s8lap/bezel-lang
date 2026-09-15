//! case: s12_9_0003
//! doc: spec
//! section: 12.9
//! title: a variance marker on a generic class is rejected
//! rule: Classes, records, and concrete generic types are always invariant
//!   -- and the variance markers are interface-only syntax: in/out on a
//!   generic parameter of a class... is a compile error.
//! cites: 12.9
//! expect: reject
//! diagnostic: variance
module Conf;

public class Box<out T>
  public constructor()
  end constructor;
end Box;
