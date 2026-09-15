//! case: s32_1_0003
//! doc: spec
//! section: 32.1
//! title: an empty extern block is a compile error
//! rule: Empty `extern` blocks are a compile error.
//! cites: 32.1
//! expect: reject
//! diagnostic: empty
module Conf;

extern "libc"
end;
