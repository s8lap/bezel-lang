//! case: s35_1_0008
//! doc: spec
//! section: 35.1
//! title: a destructor in a choice body is rejected
//! rule: Choices cannot have destructors, constructors, instance fields, block-form or
//!   storage-backed properties, indexers, or nested types — the same restriction
//!   set as enums, plus one: a choice's instance storage is its cases' payloads.
//! expect: reject
//! diagnostic: destructor
module Conf;

public choice Outcome
  Ready,
  Empty;

  destructor
  end destructor;
end Outcome;
