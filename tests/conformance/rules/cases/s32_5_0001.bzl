//! case: s32_5_0001
//! doc: spec
//! section: 32.5
//! title: every integer type, float, double and bool are FFI-compatible parameter types
//! rule: Only the following types may appear in `extern` method signatures: All integer
//!   types and `float`, `double`. `bool` -- passed as a 1-byte value.
//! cites: 32.5
//! expect: accept
module Conf;

extern "libexample"
  method Combine(a: int, b: long, c: short, d: byte, e: sbyte, f: ushort,
      g: uint, h: ulong): long;
  method Scale(x: float, y: double): double;
  method Flag(on: bool): bool;
end;
