//! case: s28_1_0004
//! doc: spec
//! section: 28.1
//! title: owned on a return type of enum type is rejected
//! rule: **Any other type** — a primitive, `string`, `Type`, a function type,
//!   an ordinary record, a value-like or borrow-bearing choice (§35.4), an
//!   enum, a non-owning inline array, or the unit type: `owned` is a
//!   **compile error**. Such values carry no disposal obligation to transfer
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
