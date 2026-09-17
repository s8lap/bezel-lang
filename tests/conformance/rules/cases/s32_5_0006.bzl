//! case: s32_5_0006
//! doc: spec
//! section: 32.5
//! title: a Bezel string parameter type is rejected
//! rule: Not allowed: Bezel `string` (use `CString` or `WString`).
//! cites: 32.5
//! expect: reject
//! diagnostic: ffi-compatible
module Conf;

extern "libexample"
  method Greet(name: string): int;
end;
