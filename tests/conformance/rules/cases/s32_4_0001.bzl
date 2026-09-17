//! case: s32_4_0001
//! doc: spec
//! section: 32.4
//! title: the convention clause selects a non-default calling convention
//! rule: The default calling convention is the platform-standard C convention. For
//!   non-default conventions, the `convention` clause specifies which. `cdecl` and
//!   `stdcall` are universally accepted.
//! cites: 32.4
//! expect: accept
module Conf;

extern "libc" convention "cdecl"
  method getpid(): int;
end;

extern "user32" convention "stdcall"
  public method MessageBoxW(hwnd: IntPtr, text: WString, caption: WString,
      style: uint): int;
end;
