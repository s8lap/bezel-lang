//! case: s35_13_0001
//! doc: spec
//! section: 35.13
//! title: a choice type in an extern signature is rejected
//! rule: Choices are not FFI-compatible: the honest C spelling of a sum carries real
//!   ABI questions the language does not promise to resolve.
//! cites: 35.13, 32.5
//! expect: reject
//! diagnostic: FFI-compatible
module Conf;

public choice Status
  Active,
  Inactive;
end Status;

extern "libc"
  public method Report(s: Status): int;   // ERROR: choices are not FFI-compatible
end;
