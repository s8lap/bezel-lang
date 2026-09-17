//! case: s35_1_0010
//! doc: spec
//! section: 35.1
//! title: an instance field in a choice body is rejected
//! rule: Choices cannot have destructors, constructors, instance fields, block-form or
//!   storage-backed properties, indexers, or nested types.
//! expect: reject
//! diagnostic: field
module Conf;

public choice Outcome
  Ready,
  Empty;

  private _flag: bool;   // ERROR: instance fields are not admitted on a choice
end Outcome;
