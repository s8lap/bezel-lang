//! case: s32_8_0018
//! doc: spec
//! section: 32.8
//! title: Interop.CodeAddress<F> rejects an F that is not an FFI-compatible function type
//! rule: `F` must be an FFI-compatible function type, gated per-instantiation in
//!   the §3.6 style.
//! cites: 32.8, 32.5
//! expect: reject
//! diagnostic: string
//! notes: valid twin is l10_6_0001's shape: F over FFI-compatible slots only.
module Conf;
import System.Interop;

public static class Handlers
  public static method Measure(s: string): int
    return 0;
  end Measure;

  public static method Address(): IntPtr
    return Interop.CodeAddress<(string) -> int>(Handlers.Measure);
  end Address;
end Handlers;
