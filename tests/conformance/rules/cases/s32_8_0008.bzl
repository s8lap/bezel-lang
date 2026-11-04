//! case: s32_8_0008
//! doc: spec
//! section: 32.8
//! title: an env marker with no paired callback is rejected
//! rule: At most one `env` marker per extern declaration, paired with exactly one
//!   function-typed parameter of the same declaration whose type contains exactly
//!   one `env` position. An `env` marker with no paired callback, a callback type
//!   with an `env` position in a declaration with no marker, two markers, or two
//!   positions — each is a compile error naming the pairing rule.
//! cites: 32.8
//! expect: reject
//! diagnostic: env
//! notes: the marker's declaration has a plain callback whose type has no env
//!   position; its valid twin is s32_8_0006's declaration, which adds the position.
module Conf;

extern "libexample"
  public method scan_rows(handle: IntPtr, sql: CString,
      cb: (int) -> int, env): int;
end;
