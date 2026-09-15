//! case: s35_1_0009
//! doc: spec
//! section: 35.1
//! title: a constructor in a choice body is rejected
//! rule: Choices cannot have destructors, constructors, instance fields, block-form or
//!   storage-backed properties, indexers, or nested types.
//! expect: reject
//! diagnostic: constructor
module Conf;

public choice Outcome
  Ready,
  Empty;

  public constructor()
  end constructor;
end Outcome;
