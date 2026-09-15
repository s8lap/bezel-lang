//! case: s32_8_0007
//! doc: spec
//! section: 32.8
//! title: env pairing does not combine with variadic(N)
//! rule: `env` pairing does not combine with `variadic(N)`: a variadic call's
//!   trailing-slot ABI is caller-shaped per call, and no corpus case pairs the two.
//! cites: 32.8, 32.11
//! expect: reject
//! diagnostic: variadic
module Conf;
import System.Interop;

extern "libexample"
  method configure(handle: IntPtr, opt: int,
      cb: (env, int) -> int, env): int
      := symbol "configure_v2" variadic(2);
end;
