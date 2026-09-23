//! case: s32_11_0003
//! doc: spec
//! section: 32.11
//! title: variadic(N) with N equal to the declared arity is the sound fixed-arity binding
//! rule: `N` equal to the arity is legal and load-bearing ("call this variadic symbol
//!   with zero variadic arguments" -- the only sound minimum-arity binding of a
//!   variadic symbol).
//! cites: 32.11
//! expect: accept
//! issue: 4561
module Conf;

extern "libc"
  method Open2(path: CString, flags: int): int
      := symbol "open" variadic(2);
end;
