//! case: s32_7_0002
//! doc: spec
//! section: 32.7
//! title: IntPtr converts explicitly to and from long
//! rule: Explicit conversions to and from `long` and `ulong` are also compiler
//!   intrinsics: `IntPtr(longValue)` and `long(ptr)` use the standard
//!   explicit-conversion syntax and are not declared as operator overloads on the
//!   record.
//! cites: 32.7, 25.4
//! expect: accept
module Conf;

public static class PointerMath
  public static method AsInteger(p: IntPtr): long
    return long(p);
  end AsInteger;

  public static method AsPointer(value: long): IntPtr
    return IntPtr(value);
  end AsPointer;
end PointerMath;
