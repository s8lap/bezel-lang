//! case: s32_1_0002
//! doc: spec
//! section: 32.1
//! title: an extern method entry omitting visibility defaults to internal
//! rule: `extern` declarations may be `public` or `internal`, following the rules for
//!   module-level declarations; omitting the keyword defaults to `internal`, the
//!   module-level default.
//! cites: 32.1, 13.2
//! expect: accept
module Conf;

extern "libm"
  method native_sqrt(x: double): double;
end;
