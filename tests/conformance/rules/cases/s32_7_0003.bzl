//! case: s32_7_0003
//! doc: spec
//! section: 32.7
//! title: IntPtr declares no arithmetic operator
//! rule: `IntPtr` is a compiler-intrinsic value type. Its members are:
//!   `IntPtr.Zero`, `=` and `<>` for null tests, and explicit conversions to and
//!   from `long` and `ulong`. `IntPtr` is opaque; no arithmetic operator is among
//!   them, and §25.4 admits no user-declared conversion or operator on it.
//! cites: 32.7, 32.12, 25.4
//! expect: reject
//! diagnostic: operator
module Conf;

public static class PointerMath
  public static method Bump(p: IntPtr): IntPtr
    return p + IntPtr(1L);
  end Bump;
end PointerMath;
