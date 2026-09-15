//! case: s32_5_0008
//! doc: spec
//! section: 32.5
//! title: Array<T> as a parameter type is rejected
//! rule: Not allowed: ... `Array<T>`, `OwningArray<T>` and the other collections.
//! cites: 32.5
//! expect: reject
//! diagnostic: ffi-compatible
module Conf;

extern "libexample"
  method Sum(values: Array<int>): int;
end;
