//! case: s32_3_0001
//! doc: spec
//! section: 32.3
//! title: a bare library name resolves per platform convention
//! rule: The library name is a bare identifier without extension. The runtime resolves
//!   it according to platform conventions.
//! cites: 32.3
//! expect: accept
module Conf;

extern "libm"
  method native_pow(radix: double, exponent: double): double;
end;
