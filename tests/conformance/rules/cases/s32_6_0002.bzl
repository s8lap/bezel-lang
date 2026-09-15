//! case: s32_6_0002
//! doc: spec
//! section: 32.6
//! title: a caller-owned string return is discharged with using var
//! rule: Native functions returning a buffer the caller must free declare their return
//!   type as `owned CString` (or `owned CString?` when the function may return
//!   NULL); ownership transfers to the Bezel side and `using var` discharges it.
//! cites: 32.6
//! expect: accept
module Conf;

extern "libc"
  method strdup(s: CString): owned CString?;
end;

public static class Strings
  public static method DuplicateLength(s: CString): int
    using var copy := strdup(s);
    if copy = null then
      return 0;
    end;
    return copy.Length;
  end DuplicateLength;
end Strings;
