//! case: s28_1_0004
//! doc: spec
//! section: 28.1
//! title: owned on a return type of enum type is rejected
//! rule: On a primitive, `string`, a function type, an ordinary record, an enum, or the
//!   unit type, `owned` is a compile error: such values carry no disposal
//!   obligation to transfer.
//! expect: reject
//! diagnostic: owned
module Conf;

enum Status
  Ready, Busy;
end Status;

public class Factory
  public method Acquire(): owned Status
    return Status.Ready;
  end Acquire;
end Factory;
