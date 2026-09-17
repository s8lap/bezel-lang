//! case: s22_6_0018
//! doc: spec
//! section: 22.6
//! title: an owned/ordinary pair over a where-T-IDisposable constraint passes the declaration guard
//! rule: "A pair differing only by ordinary `owned` is declarable exactly
//!   when, at every differing position, the declared type ... is a class
//!   type, an interface type, or a type parameter whose constraints
//!   exclude every substitution at which the pair collapses ... `where T:
//!   IDisposable` qualifies, its closure guaranteed by §8.7."
//! cites: 22.6, 28.1, 14.2
//! expect: accept
module Conf;

public class Box<T> where T: IDisposable
  public method Put(item: T)
  end Put;

  public method Put(owned item: T)
  end Put;
end Box;
