//! case: s28_1_0009
//! doc: spec
//! section: 28.1
//! title: owned on a bare unconstrained type parameter is admitted
//! rule: `owned` requires the declared type, nullability stripped, to be a class type,
//!   an interface type, or a bare type parameter; at a bare type parameter the
//!   generic body type-checks under the class-or-interface assumption.
//! cites: 28.3
//! expect: accept
module Conf;

public class Factory
  public method Adopt<T>(owned item: T)
    using var i := move item;
  end Adopt;
end Factory;
