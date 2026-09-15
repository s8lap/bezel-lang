//! case: s10_5_0001
//! doc: spec
//! section: 10.5
//! title: an enum cannot declare an instance field
//! rule: An enum may not declare instance fields (the runtime representation
//!   is the underlying integer).
//! cites: 10.5
//! expect: reject
//! diagnostic: instance field
module Conf;

enum Priority
  Low, Medium, High;

  private _extra: int;
end Priority;
