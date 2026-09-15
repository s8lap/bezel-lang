//! case: s32_11_0004
//! doc: spec
//! section: 32.11
//! title: variadic(N) with N less than the declared arity passes the trailing parameters variadically
//! rule: `variadic(N)` on the symbol clause declares: the named symbol is a C-variadic
//!   function whose fixed parameters are the first N declared parameters; the
//!   remaining declared parameters pass in the symbol's variadic slots.
//! cites: 32.11
//! expect: accept
module Conf;

extern "libc"
  method OpenCreate(path: CString, flags: int, mode: uint): int
      := symbol "open" variadic(2);
end;
