//! case: s28_14_0006
//! doc: spec
//! section: 28.14
//! title: a call-bounded callee may not store the callback in a field
//! rule: A function-typed parameter declared without scoped is
//!   call-bounded; the callback may not be stored in a field of any
//!   object.
//! cites: 28.14
//! expect: reject
//! diagnostic: store
module Conf;

public class Runner
  private _cb: () -> ();

  public method Register(cb: () -> ())
    _cb := cb;
  end Register;
end Runner;
