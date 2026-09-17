//! case: s32_5_0016
//! doc: spec
//! section: 32.5
//! title: an owned parameter is not admitted on an extern method
//! rule: Not admitted, with reasons: ... `owned` parameters (no native contract for
//!   consuming a Bezel-owned argument).
//! cites: 32.5
//! expect: reject
//! diagnostic: owned
module Conf;

extern "libexample"
  method Consume(owned payload: CString): int;
end;
