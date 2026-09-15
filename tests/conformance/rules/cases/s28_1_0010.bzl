//! case: s28_1_0010
//! doc: spec
//! section: 28.1
//! title: owned on a bare type parameter constrained to value is rejected at the declaration
//! rule: A declaration whose constraints exclude class and interface substitutions
//!   outright (`where T: value`) rejects the `owned` marker at the declaration — it
//!   could never be more than redundant.
//! expect: reject
//! diagnostic: owned
module Conf;

public class Factory
  public method Adopt<T>(owned item: T) where T: value
  end Adopt;
end Factory;
