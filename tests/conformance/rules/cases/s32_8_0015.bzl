//! case: s32_8_0015
//! doc: spec
//! section: 32.8
//! title: a nullable env-paired callback admits null
//! rule: A **nullable** paired callback type (`((env, …) -> int)?`) follows the
//!   optional-callback rule (§32.5): passing `null` passes C `NULL` in the
//!   function-pointer slot and `NULL` in the `env` slot.
//! cites: 32.8, 32.5
//! expect: accept
module Conf;

extern "libexample"
  public method scan_rows(handle: IntPtr, sql: CString,
      cb: ((env, int) -> int)?, env): int;
end;

public static class Scanner
  public static method RunWithoutCallback(handle: IntPtr, sql: CString): int
    return scan_rows(handle, sql, null);
  end RunWithoutCallback;
end Scanner;
