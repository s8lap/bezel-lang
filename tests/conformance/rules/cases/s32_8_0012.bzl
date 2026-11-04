//! case: s32_8_0012
//! doc: spec
//! section: 32.8
//! title: the env slot is not a Bezel-visible argument, so passing it explicitly is an arity error
//! rule: The Bezel-visible signature of the extern **omits** the `env` parameter, and
//!   the callback's Bezel-visible function type omits the `env` position — the
//!   programmer never sees, mints, or stores the environment address.
//! cites: 32.8
//! expect: reject
//! diagnostic: argument
//! notes: valid twin is s32_8_0006, which calls the same shape with the env slot
//!   omitted (three Bezel arguments).
module Conf;

extern "libexample"
  public method scan_rows(handle: IntPtr, sql: CString,
      cb: (env, int) -> int, env): int;
end;

public static class Scanner
  public static method CountAbove(handle: IntPtr, sql: CString,
      threshold: int): int
    return scan_rows(handle, sql,
        lambda(code: int): int => int(code > threshold), IntPtr.Zero);
  end CountAbove;
end Scanner;
