//! case: s32_5_0011
//! doc: spec
//! section: 32.5
//! title: a RawBytes extern return type is rejected
//! rule: A `RawBytes` extern return type is rejected (a C function returns one value,
//!   and the two-slot shape has no return convention).
//! cites: 32.5
//! expect: reject
//! diagnostic: return type
module Conf;

extern "libexample"
  method LoadPayload(id: int): RawBytes;
end;
