//! case: s28_1_0009
//! doc: spec
//! section: 28.1
//! title: owned on a bare unconstrained type parameter is admitted
//! rule: `owned` on a bare type parameter's meaning is fixed per instantiation: the
//!   generic body type-checks under the class-or-interface assumption, and marker-
//!   keyed forms written under that assumption remain admissible.
//! cites: 28.3
//! expect: accept
module Conf;

public class Factory
  public method Adopt<T>(owned item: T)
    using var i := move item;
  end Adopt;
end Factory;
