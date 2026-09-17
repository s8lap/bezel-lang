//! case: s32_5_0019
//! doc: spec
//! section: 32.5
//! title: a callback may not capture a binding passed out to the same extern call
//! rule: A callback argument of the same extern call may not capture a binding passed
//!   `out` to that call -- a compile error.
//! cites: 32.5, 32.8
//! expect: reject
//! diagnostic: capture
module Conf;
import System.Interop;

extern "libexample"
  method Scan(handle: IntPtr, out count: int,
      cb: (env, int) -> (), env): int;
end;

public static class Scanner
  public static method BadScan(h: IntPtr): int
    return Scan(h, out var n, lambda(code: int) => do
      var seen := n;
    end);
  end BadScan;
end Scanner;
