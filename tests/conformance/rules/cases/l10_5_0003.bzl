//! case: l10_5_0003
//! doc: stdlib
//! section: 10.5
//! title: a single-level Ptr<T> over an admitted pointee type is admitted
//! rule: T is gated (per-instantiation ...) to the FFI-POINTEE SET:
//!   integer types, float, double, bool, IntPtr, UIntPtr, and
//!   C-compatible records.
//! cites: 10.5
//! expect: accept
module Conf;

import System.Interop;

public class Program
  public method Demo()
    var p: Ptr<int>;
  end Demo;
end Program;
