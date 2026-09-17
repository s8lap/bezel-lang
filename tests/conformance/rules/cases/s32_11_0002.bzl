//! case: s32_11_0002
//! doc: spec
//! section: 32.11
//! title: several declarations of different shapes may bind one symbol
//! rule: Several declarations, of any mix of shapes, may bind one symbol.
//! cites: 32.11
//! expect: accept
module Conf;

extern "libcurl"
  method SetOptLong(h: IntPtr, opt: int, value: long): int
      := symbol "curl_easy_setopt" variadic(2);
  method SetOptStr(h: IntPtr, opt: int, value: CString): int
      := symbol "curl_easy_setopt" variadic(2);
end;
