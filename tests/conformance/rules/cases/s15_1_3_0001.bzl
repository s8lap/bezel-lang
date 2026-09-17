//! case: s15_1_3_0001
//! doc: spec
//! section: 15.1.3
//! title: an inline array of FFI-compatible scalars is admitted as an extern parameter
//! rule: Inline arrays of FFI-compatible scalar elements may appear as
//!   `extern` method parameters under the by-address rule (§15.1.3).
//! cites: 15.1.3
//! expect: accept
module Conf;

extern "libc"
  public method fill(buf: [16]byte, len: int): int;
end;
