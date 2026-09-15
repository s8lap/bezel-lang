//! case: s32_5_0015
//! doc: spec
//! section: 32.5
//! title: out owned is not admitted on an extern parameter
//! rule: Not admitted, with reasons: `out owned` (ownership transfer through an
//!   out-slot has no defined native contract).
//! cites: 32.5
//! expect: reject
//! diagnostic: out owned
module Conf;

extern "libexample"
  method Acquire(out owned handle: CString): int;
end;
