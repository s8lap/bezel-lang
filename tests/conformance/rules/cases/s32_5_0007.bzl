//! case: s32_5_0007
//! doc: spec
//! section: 32.5
//! title: an enum parameter type is rejected
//! rule: Not allowed: ... enums (declare the underlying integer type and cast
//!   explicitly at the boundary).
//! cites: 32.5, 5.4
//! expect: reject
//! diagnostic: ffi-compatible
module Conf;

public enum Mode
  Read, Write;
end Mode;

extern "libexample"
  method SetMode(m: Mode): int;
end;
