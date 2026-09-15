//! case: s32_6_0003
//! doc: spec
//! section: 32.6
//! title: a borrowed IntPtr return is read with string.FromCStringPointer
//! rule: For native functions returning a string pointer that the caller does not own
//!   ... the FFI declaration's return type is `IntPtr`, not `CString`. The Bezel
//!   side reads through the pointer with `string.FromCStringPointer(p: IntPtr):
//!   string`, which copies into a fresh Bezel `string` without taking ownership of
//!   the original buffer.
//! cites: 32.6
//! expect: accept
module Conf;

extern "libc"
  method getenv(name: CString): IntPtr;
end;

public static class Env
  public static method GetVariable(name: string): string?
    using var key := CString.From(name);
    var resultPtr := getenv(key);
    if resultPtr = IntPtr.Zero then
      return null;
    end;
    return string.FromCStringPointer(resultPtr);
  end GetVariable;
end Env;
