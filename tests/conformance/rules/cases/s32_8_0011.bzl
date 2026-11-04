//! case: s32_8_0011
//! doc: spec
//! section: 32.8
//! title: a callback type with two env positions is rejected
//! rule: An `env` marker with no paired callback, a callback type with an `env`
//!   position in a declaration with no marker, two markers, or two positions — each
//!   is a compile error naming the pairing rule.
//! cites: 32.8
//! expect: reject
//! diagnostic: env
//! notes: valid twin is s32_8_0006's declaration, with exactly one position.
module Conf;

extern "libexample"
  public method scan_rows(handle: IntPtr, sql: CString,
      cb: (env, int, env) -> int, env): int;
end;
