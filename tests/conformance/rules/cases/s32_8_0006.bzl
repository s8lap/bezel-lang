//! case: s32_8_0006
//! doc: spec
//! section: 32.8
//! title: a capturing lambda is admitted at an env-paired callback parameter
//! rule: The pair makes the callback parameter capture-admitting: any function value --
//!   including a capturing lambda ... -- may be passed there.
//! cites: 32.8
//! expect: accept
//! issue: 4561
module Conf;
import System.Interop;

extern "libexample"
  method scan_rows(handle: IntPtr, sql: CString,
      cb: (env, int) -> int, env): int;
end;

public static class Scanner
  public static method CountAbove(handle: IntPtr, sql: CString,
      threshold: int): int
    return scan_rows(handle, sql,
        lambda(code: int): int => int(code > threshold));
  end CountAbove;
end Scanner;
