//! case: s32_12_0004
//! doc: spec
//! section: 32.12
//! title: Ptr<T> is admitted at a C-compatible record pointee
//! rule: `T` is drawn from the FFI-pointee set: integer types, `float`, `double`,
//!   `bool`, `IntPtr`, `UIntPtr`, and C-compatible records.
//! cites: 32.12, 32.13
//! expect: accept
module Conf;
import System.Interop;

public record TimeSpec(Sec: long, Nsec: long);

public static class Clock
  public static method Peek(p: Ptr<TimeSpec>): TimeSpec
    return p.Load();
  end Peek;
end Clock;
