//! case: s32_5_0004
//! doc: spec
//! section: 32.5
//! title: an inline array of an FFI-compatible scalar is admitted as an extern parameter by address
//! rule: Inline arrays `[N]T` where the element type is an FFI-compatible scalar ...
//!   Admitted as parameters only, by address.
//! cites: 32.5, 15.1.3
//! expect: accept
module Conf;

extern "libexample"
  method Fill(buf: [4]int): ();
end;
