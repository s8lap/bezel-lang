//! case: s32_8_0014
//! doc: spec
//! section: 32.8
//! title: a non-env parameter of a paired callback type must still be FFI-compatible
//! rule: The non-`env` parameter and return types of the paired callback follow
//!   §32.5's function-type rule unchanged.
//! cites: 32.8, 32.5
//! expect: reject
//! diagnostic: string
//! notes: valid twin is s32_8_0006's declaration, where the non-env slot is int.
module Conf;

extern "libexample"
  public method scan_rows(handle: IntPtr, sql: CString,
      cb: (env, string) -> int, env): int;
end;
