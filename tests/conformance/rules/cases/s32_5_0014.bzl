//! case: s32_5_0014
//! doc: spec
//! section: 32.5
//! title: ref and out are admitted on FFI-compatible scalar extern parameters
//! rule: On extern parameters whose type is an FFI-compatible scalar ...: `ref p: T` --
//!   C sees `T*`, receiving the address of the caller's storage. `out p: T` -- same
//!   address passing.
//! cites: 32.5
//! expect: accept
module Conf;

extern "libc"
  method waitpid(pid: int, out status: int, options: int): int;
  method modf(x: double, ref iptr: double): double;
end;
