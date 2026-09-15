//! case: s32_7_0001
//! doc: spec
//! section: 32.7
//! title: IntPtr.Zero supports the null test
//! rule: `IntPtr.Zero` -- the null pointer (integer 0). `=` and `<>` for null tests.
//! cites: 32.7
//! expect: accept
module Conf;

extern "libdl"
  method dlopen(filename: CString, flags: int): IntPtr;
end;

public static class Loader
  public static method IsLoaded(filename: CString, flags: int): bool
    var handle := dlopen(filename, flags);
    return handle <> IntPtr.Zero;
  end IsLoaded;
end Loader;
