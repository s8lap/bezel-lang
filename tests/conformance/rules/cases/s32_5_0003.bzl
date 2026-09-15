//! case: s32_5_0003
//! doc: spec
//! section: 32.5
//! title: CString is admitted as a parameter and an owned nullable return
//! rule: `CString` -- null-terminated UTF-8 byte sequence.
//! cites: 32.5, 32.6
//! expect: accept
module Conf;

extern "libc"
  method getenv(name: CString): IntPtr;
  method strdup(s: CString): owned CString?;
end;
