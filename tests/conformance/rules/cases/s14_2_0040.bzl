//! case: s14_2_0040
//! doc: spec
//! section: 14.2
//! title: a class bound beside value admits no type by language rule and is rejected
//! rule: A constraint combination that admits no type — `class, value`;
//!   `value` with `IDisposable` or an interface extending it ... and every
//!   other combination whose admitted sets are disjoint as a matter of
//!   language rule — is a compile error at the declaration.
//! cites: 14.2
//! expect: reject
//! diagnostic: admits no type
module Conf;

public sealed class Leaf
  public constructor()
  end constructor;
end Leaf;

public class Holder<T> where T: Leaf, value
  public constructor()
  end constructor;
end Holder;
