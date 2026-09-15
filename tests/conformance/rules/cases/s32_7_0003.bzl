//! case: s32_7_0003
//! doc: spec
//! section: 32.7
//! title: IntPtr declares no arithmetic operator
//! rule: `IntPtr` is opaque. Bezel provides no language-level operation to dereference
//!   it; typed access to foreign memory goes through `Ptr<T>`.
//! cites: 32.7, 32.12
//! expect: reject
//! diagnostic: operator
module Conf;

public static class PointerMath
  public static method Bump(p: IntPtr): IntPtr
    return p + IntPtr(1L);
  end Bump;
end PointerMath;
