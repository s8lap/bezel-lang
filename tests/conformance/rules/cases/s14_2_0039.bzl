//! case: s14_2_0039
//! doc: spec
//! section: 14.2
//! title: a sealed class bound beside an interface the class does not implement is well-formed
//! rule: A combination empty only by declaration — a class bound naming a
//!   `sealed` class beside an interface that class does not implement — is
//!   not: it is well-formed, and no type argument satisfies it until the
//!   declarations it names change.
//! cites: 14.2
//! expect: accept
module Conf;

public interface IShape
  method Area(): int;
end IShape;

public sealed class Leaf
  public constructor()
  end constructor;
end Leaf;

public class Holder<T> where T: Leaf, IShape
  public constructor()
  end constructor;
end Holder;
