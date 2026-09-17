//! case: s32_5_0012
//! doc: spec
//! section: 32.5
//! title: an inline array extern return type is rejected
//! rule: `[N]T` as an extern return type is rejected (C functions do not return arrays;
//!   declare `Ptr<U>`, or a C-compatible record with an inline-array field, per the
//!   actual C signature).
//! cites: 32.5, 15.1.3
//! expect: reject
//! diagnostic: return type
module Conf;

extern "libexample"
  method MakeBuffer(size: int): [4]int;
end;
