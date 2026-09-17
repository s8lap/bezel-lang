//! case: s10_5_0002
//! doc: spec
//! section: 10.5
//! title: a static readonly field is admitted where an instance field is not
//! rule: An enum may declare `static readonly` fields; instance fields are
//!   forbidden because the runtime representation is the underlying integer.
//! cites: 10.5, 10.4
//! expect: accept
module Conf;

enum Priority
  Low, Medium, High;

  public static readonly Extra: int := 5;
end Priority;
